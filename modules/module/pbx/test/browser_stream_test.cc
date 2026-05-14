#include "browser_stream.hpp"
#include "sip_bridge.hpp"
#include "sip_frame.hpp"
#include "wsframe.hpp"

#include "ace/Time_Value.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Test fake — records every frame the bridge sends downstream.
class FakeTunnel : public TunnelSink {
public:
  struct Sent {
    SipFrame::Op op;
    std::uint32_t stream_id;
    std::string payload;
  };
  std::vector<Sent> sent;
  void send_frame(SipFrame::Op op, std::uint32_t sid,
                  const std::string &payload) override {
    sent.push_back({op, sid, payload});
  }
};

// Helpers: build WS frames the way a SIP.js client would (text, masked).
std::vector<std::uint8_t> ws_text_masked(const std::string &payload) {
  return wsframe::encode(std::vector<std::uint8_t>(payload.begin(),
                                                     payload.end()),
                          /*opcode=*/0x1, /*mask=*/true);
}

std::vector<std::uint8_t> ws_ping_masked() {
  // wsframe::encode with PING opcode + mask.
  return wsframe::encode({}, /*opcode=*/0x9, /*mask=*/true);
}

std::vector<std::uint8_t> ws_close_masked() {
  return wsframe::encode({}, /*opcode=*/0x8, /*mask=*/true);
}

/// Returns a pair of connected sockets via socketpair(AF_UNIX,SOCK_STREAM).
/// fd[0] is the "browser side", fd[1] is the "stream side". Tests close
/// fd[0] themselves; fd[1] is owned by BrowserStream once constructed.
void make_socketpair(int fds[2]) {
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      << "socketpair failed: " << std::strerror(errno);
}

/// Drain whatever bytes are pending on @p fd into a buffer. Non-blocking;
/// uses MSG_DONTWAIT.
std::vector<std::uint8_t> drain_socket(int fd) {
  std::vector<std::uint8_t> out;
  std::uint8_t buf[4096];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0) break;
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

} // namespace

// ── Construction wires bridge ──────────────────────────────────────────────

TEST(BrowserStream, ConstructorRegistersStreamWithBridge)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], R"({"sip":"u_alice"})");

    EXPECT_EQ(1u, bridge.active_streams());
    EXPECT_GT(s->stream_id(), 0u);
    // OPEN frame went down the tunnel.
    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::OPEN,            tun.sent[0].op);
    EXPECT_EQ(R"({"sip":"u_alice"})",        tun.sent[0].payload);

    // Cleanup — destructor closes fd[1] and notifies bridge.
    delete s;
    EXPECT_EQ(0u, bridge.active_streams());
    ::close(fds[0]);
}

// ── Browser → bridge: WS frame in → SipBridge::on_browser_data ─────────────

TEST(BrowserStream, OnInput_DecodesAndDispatches)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");
    tun.sent.clear();   // discard OPEN

    const std::string sip_msg =
        "REGISTER sip:society SIP/2.0\r\nVia: SIP/2.0/WSS x\r\n\r\n";
    const auto frame = ws_text_masked(sip_msg);
    ssize_t w = ::send(fds[0], frame.data(), frame.size(), 0);
    ASSERT_EQ(static_cast<ssize_t>(frame.size()), w);

    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::DATA,    tun.sent[0].op);
    EXPECT_EQ(s->stream_id(),        tun.sent[0].stream_id);
    EXPECT_EQ(sip_msg,               tun.sent[0].payload);

    delete s;
    ::close(fds[0]);
}

TEST(BrowserStream, OnInput_PingTriggersPong)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");

    const auto ping = ws_ping_masked();
    ::send(fds[0], ping.data(), ping.size(), 0);

    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    // PONG bytes should have hit the peer.
    const auto reply = drain_socket(fds[0]);
    ASSERT_GE(reply.size(), 2u);
    // First byte: FIN=1, opcode=0xA (pong). No mask from server.
    EXPECT_EQ(0x8A, reply[0]);

    delete s;
    ::close(fds[0]);
}

// ── Keep-alive: handle_timeout emits a WS ping frame ─────────────────────

TEST(BrowserStream, HandleTimeout_EmitsWsPing)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");

    // Production arms the timer in register_with_reactor() (skipped on the
    // test path); fire the callback directly to exercise the ping write.
    EXPECT_EQ(0, s->handle_timeout(ACE_Time_Value::zero, nullptr));

    const auto frame = drain_socket(fds[0]);
    ASSERT_GE(frame.size(), 2u);
    // FIN=1, opcode=0x9 (ping), unmasked (server→client).
    EXPECT_EQ(0x89, frame[0]);

    delete s;
    ::close(fds[0]);
}

TEST(BrowserStream, OnInput_CloseFrame_TellsBridge)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");
    tun.sent.clear();

    const auto cls = ws_close_masked();
    ::send(fds[0], cls.data(), cls.size(), 0);

    EXPECT_EQ(-1, s->handle_input(s->get_handle()));

    // Bridge was told the browser closed.
    EXPECT_EQ(0u, bridge.active_streams());
    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::CLOSE,    tun.sent[0].op);
    EXPECT_EQ("browser_closed",       tun.sent[0].payload);

    delete s;
    ::close(fds[0]);
}

// ── Bridge → browser: BrowserSink::send_bytes writes WS-framed bytes ──────

TEST(BrowserStream, SendBytes_EncodesWsFrame)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");

    const std::string sip_msg = "SIP/2.0 200 OK\r\n\r\n";
    s->send_bytes(sip_msg);

    // Peer should see a single WS text frame containing the SIP bytes.
    const auto bytes = drain_socket(fds[0]);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(0x81, bytes[0])
        << "FIN=1, opcode=0x1 (text), unmasked (server→client)";

    // Decode it back through wsframe to confirm payload is intact.
    std::vector<std::uint8_t> buf(bytes.begin(), bytes.end());
    auto f = wsframe::decode(buf);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(0x1, f->opcode);
    EXPECT_EQ(sip_msg,
              std::string(f->payload.begin(), f->payload.end()));

    delete s;
    ::close(fds[0]);
}

// ── Bridge → browser: BrowserSink::close sends WS close frame ─────────────

TEST(BrowserStream, Close_SendsWsCloseFrame)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");

    s->close("session_timeout");

    const auto bytes = drain_socket(fds[0]);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(0x88, bytes[0])
        << "FIN=1, opcode=0x8 (close), unmasked";

    // After close, send_bytes is a no-op (socket already shut).
    s->send_bytes("anything");
    EXPECT_TRUE(drain_socket(fds[0]).empty());

    delete s;
    ::close(fds[0]);
}

// ── Multiple frames in one read ────────────────────────────────────────────

TEST(BrowserStream, MultipleFramesInOneRead)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");
    tun.sent.clear();

    const auto a = ws_text_masked("AAA");
    const auto b = ws_text_masked("BBBB");
    std::vector<std::uint8_t> blob;
    blob.insert(blob.end(), a.begin(), a.end());
    blob.insert(blob.end(), b.begin(), b.end());
    ::send(fds[0], blob.data(), blob.size(), 0);

    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    ASSERT_EQ(2u, tun.sent.size());
    EXPECT_EQ("AAA",  tun.sent[0].payload);
    EXPECT_EQ("BBBB", tun.sent[1].payload);

    delete s;
    ::close(fds[0]);
}

// ── Partial frame across reads ─────────────────────────────────────────────

TEST(BrowserStream, PartialFrameAcrossReads)
{
    FakeTunnel  tun;
    SipBridge   bridge(&tun);

    int fds[2]; make_socketpair(fds);
    auto *s = new BrowserStream(bridge, fds[1], "{}");
    tun.sent.clear();

    const auto frame = ws_text_masked("hello-world");
    const std::size_t split = 5;
    ASSERT_GT(frame.size(), split);

    // First half — handle_input drains, sees no complete frame.
    ::send(fds[0], frame.data(), split, 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));
    EXPECT_EQ(0u, tun.sent.size());

    // Second half — frame completes; bridge sees one DATA call.
    ::send(fds[0], frame.data() + split, frame.size() - split, 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));
    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ("hello-world", tun.sent[0].payload);

    delete s;
    ::close(fds[0]);
}

#include "agent_stream.hpp"
#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"
#include "sip_frame.hpp"
#include "wsframe.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
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

// WS frames the AGENT (client side) would send — masked.
std::vector<std::uint8_t> ws_text_masked(const std::string &payload) {
  return wsframe::encode(std::vector<std::uint8_t>(payload.begin(),
                                                     payload.end()),
                          /*opcode=*/0x1, /*mask=*/true);
}

std::vector<std::uint8_t> ws_ping_masked() {
  return wsframe::encode({}, /*opcode=*/0x9, /*mask=*/true);
}

std::vector<std::uint8_t> ws_close_masked() {
  return wsframe::encode({}, /*opcode=*/0x8, /*mask=*/true);
}

void make_socketpair(int fds[2]) {
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      << "socketpair failed: " << std::strerror(errno);
}

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

// ── Construction attaches AgentStream to the endpoint ────────────────────

TEST(AgentStream, ConstructorAttachesToEndpoint)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    EXPECT_TRUE(cte.has_agent());

    // Teardown: delete the stream — runs notify_disconnect_once which
    // tells the endpoint we're gone.
    delete s;
    EXPECT_FALSE(cte.has_agent());
    ::close(fds[0]);
}

// ── Agent → endpoint: WS frame in → CloudTunnelEndpoint::on_bytes_received ──

TEST(AgentStream, OnInput_ForwardsToEndpoint)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    // Wire a browser on the bridge so DATA frames have somewhere to land.
    class FakeBrowser : public BrowserSink {
    public:
      std::vector<std::string> got;
      bool closed = false;
      void send_bytes(const std::string &b) override { got.push_back(b); }
      void close(const std::string &) override { closed = true; }
    };
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");
    tun.sent.clear();  // discard the OPEN frame that just went down

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    // Build a tunnel-protocol DATA frame and ship it through the agent
    // WS as a text frame (RFC 7118).
    const std::string tunnel_data_frame =
        SipFrame::encode(SipFrame::Op::DATA, sid, "agent-reply-bytes");
    const auto ws = ws_text_masked(tunnel_data_frame);
    ::send(fds[0], ws.data(), ws.size(), 0);

    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    ASSERT_EQ(1u, b.got.size());
    EXPECT_EQ("agent-reply-bytes", b.got[0]);

    delete s;
    ::close(fds[0]);
}

TEST(AgentStream, OnInput_PingTriggersPong)
{
    CloudTunnelEndpoint cte;
    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    const auto ping = ws_ping_masked();
    ::send(fds[0], ping.data(), ping.size(), 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    const auto reply = drain_socket(fds[0]);
    ASSERT_GE(reply.size(), 2u);
    // FIN=1, opcode=0xA (pong), unmasked (server→client).
    EXPECT_EQ(0x8A, reply[0]);

    delete s;
    ::close(fds[0]);
}

TEST(AgentStream, OnInput_CloseFrame_DisconnectsEndpoint)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);
    EXPECT_TRUE(cte.has_agent());

    const auto cls = ws_close_masked();
    ::send(fds[0], cls.data(), cls.size(), 0);
    EXPECT_EQ(-1, s->handle_input(s->get_handle()));

    EXPECT_FALSE(cte.has_agent())
        << "endpoint must be released on inbound close frame";

    delete s;
    ::close(fds[0]);
}

// ── Endpoint → agent: send_frame writes WS-framed bytes to the socket ────

TEST(AgentStream, EndpointSendFrame_WritesWsFrameToSocket)
{
    CloudTunnelEndpoint cte;
    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    // Endpoint sends a CDR_PUSH frame to the agent.
    cte.send_frame(SipFrame::Op::CDR_PUSH, 0, R"({"_id":"c1"})");

    // Peer (the test's "agent side") should see a WS text frame
    // containing the tunnel-protocol bytes for that frame.
    const auto bytes = drain_socket(fds[0]);
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(0x81, bytes[0])
        << "FIN=1, opcode=0x1 (text), unmasked (server→client)";

    // Decode the WS frame and confirm payload is the SipFrame::encode bytes.
    std::vector<std::uint8_t> buf(bytes.begin(), bytes.end());
    auto f = wsframe::decode(buf);
    ASSERT_TRUE(f.has_value());
    const std::string ws_payload(f->payload.begin(), f->payload.end());
    const auto inner = SipFrame::decode(ws_payload);
    ASSERT_EQ(SipFrame::Status::Ok,        inner.status);
    EXPECT_EQ(SipFrame::Op::CDR_PUSH,      inner.frame.op);
    EXPECT_EQ(R"({"_id":"c1"})",           inner.frame.payload);

    delete s;
    ::close(fds[0]);
}

// ── Endpoint releases transport → AgentStream's socket is closed ─────────

TEST(AgentStream, EndpointReleasesTransport_ClosesSocket)
{
    CloudTunnelEndpoint cte;
    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);
    EXPECT_TRUE(cte.has_agent());

    // Endpoint decides to drop the agent (e.g. saw a protocol error).
    cte.on_agent_disconnected();

    EXPECT_FALSE(cte.has_agent());

    // AgentStream's socket should be closed; sending from the peer end
    // should yield 0 bytes on a subsequent read.
    const std::string probe = "anything";
    ::send(fds[0], probe.data(), probe.size(), 0);

    // recv from fds[1] should return 0 (peer closed) because AgentStream
    // closed its end via the adapter.close path.
    char buf[16];
    ssize_t n = ::recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);
    // fds[1] is owned by AgentStream's m_stream, which is now closed.
    // recv on a closed fd returns -1 (EBADF) — either way it's NOT > 0.
    EXPECT_LE(n, 0);

    delete s;
    ::close(fds[0]);
}

// ── Multiple frames in one read ──────────────────────────────────────────

TEST(AgentStream, MultipleFramesInOneRead)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    class FakeBrowser : public BrowserSink {
    public:
      std::vector<std::string> got;
      void send_bytes(const std::string &b) override { got.push_back(b); }
      void close(const std::string &) override {}
    };
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    const auto a = ws_text_masked(
        SipFrame::encode(SipFrame::Op::DATA, sid, "AAA"));
    const auto bf = ws_text_masked(
        SipFrame::encode(SipFrame::Op::DATA, sid, "BBBB"));
    std::vector<std::uint8_t> blob;
    blob.insert(blob.end(), a.begin(),  a.end());
    blob.insert(blob.end(), bf.begin(), bf.end());
    ::send(fds[0], blob.data(), blob.size(), 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));

    ASSERT_EQ(2u, b.got.size());
    EXPECT_EQ("AAA",  b.got[0]);
    EXPECT_EQ("BBBB", b.got[1]);

    delete s;
    ::close(fds[0]);
}

// ── Partial frame across reads ───────────────────────────────────────────

TEST(AgentStream, PartialFrameAcrossReads)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    class FakeBrowser : public BrowserSink {
    public:
      std::vector<std::string> got;
      void send_bytes(const std::string &b) override { got.push_back(b); }
      void close(const std::string &) override {}
    };
    FakeBrowser b;
    const auto sid = bridge.on_browser_upgrade(&b, "{}");

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);

    const auto frame = ws_text_masked(
        SipFrame::encode(SipFrame::Op::DATA, sid, "hello-world"));
    const std::size_t split = 5;
    ASSERT_GT(frame.size(), split);

    ::send(fds[0], frame.data(), split, 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));
    EXPECT_EQ(0u, b.got.size());

    ::send(fds[0], frame.data() + split, frame.size() - split, 0);
    EXPECT_EQ(0, s->handle_input(s->get_handle()));
    ASSERT_EQ(1u, b.got.size());
    EXPECT_EQ("hello-world", b.got[0]);

    delete s;
    ::close(fds[0]);
}

// ── Invalid SipFrame from agent drops the tunnel ─────────────────────────

TEST(AgentStream, OnInput_InvalidSipFrame_DropsAgent)
{
    FakeTunnel  tun;
    CloudTunnelEndpoint cte;
    SipBridge   bridge(&cte);
    cte.attach_bridge(&bridge);

    int fds[2]; make_socketpair(fds);
    auto *s = new AgentStream(cte, fds[1]);
    ASSERT_TRUE(cte.has_agent());

    // Send a WS frame whose payload is a malformed SipFrame (bad version).
    std::string bad = SipFrame::encode(SipFrame::Op::DATA, 1, "x");
    bad[0] = 0x77;
    const auto ws = ws_text_masked(bad);
    ::send(fds[0], ws.data(), ws.size(), 0);

    // handle_input returns -1 because the endpoint dropped us mid-loop.
    EXPECT_EQ(-1, s->handle_input(s->get_handle()));
    EXPECT_FALSE(cte.has_agent());

    delete s;
    ::close(fds[0]);
}

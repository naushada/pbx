#include "asterisk_ws_factory.hpp"
#include "sip_bridge.hpp"
#include "sip_frame.hpp"
#include "sip_frame_demux.hpp"
#include "wsframe.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

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

/// Factory that hands out a single, pre-staged adapter on its first
/// `open()` call. Lets tests construct an `AsteriskStream` first
/// (with the demux's reference) and then route it through the demux's
/// `on_tunnel_bytes(OPEN)` machinery so the stream ends up in
/// `m_streams[sid]`.
class OneShotFactory : public IAsteriskFactory {
public:
  std::unique_ptr<IAsteriskStream> next;
  std::unique_ptr<IAsteriskStream> open(std::uint32_t,
                                         const std::string &) override {
    return std::move(next);
  }
};

// Server → us (Asterisk → agent): unmasked WS frames.
std::vector<std::uint8_t> ws_text_unmasked(const std::string &payload) {
  return wsframe::encode(std::vector<std::uint8_t>(payload.begin(),
                                                     payload.end()),
                          /*opcode=*/0x1, /*mask=*/false);
}
std::vector<std::uint8_t> ws_ping_unmasked() {
  return wsframe::encode({}, /*opcode=*/0x9, /*mask=*/false);
}
std::vector<std::uint8_t> ws_close_unmasked() {
  return wsframe::encode({}, /*opcode=*/0x8, /*mask=*/false);
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

/// Common scaffold: a FakeTunnel + OneShotFactory + SipFrameDemux,
/// already wired so that handing `oneshot.next = as->make_adapter()`
/// and driving an OPEN frame inserts `as`'s adapter into the demux's
/// `m_streams[sid]`. Returns the file descriptor pair the test owns
/// (the test must close fds[0]; fds[1] is owned by the AsteriskStream
/// once constructed).
struct DemuxFixture {
  FakeTunnel       tun;
  OneShotFactory   factory;
  SipFrameDemux    demux{&tun, factory};
};

} // namespace

// ── build_upgrade_request ────────────────────────────────────────────────────

TEST(AsteriskWsFactory, BuildUpgradeRequest_HasWsHeaders)
{
    auto [req, key] = AsteriskStream::build_upgrade_request("127.0.0.1", "/ws");
    EXPECT_NE(std::string::npos, req.find("GET /ws HTTP/1.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Host: 127.0.0.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Upgrade: websocket\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: Upgrade\r\n"));
    EXPECT_NE(std::string::npos, req.find("Sec-WebSocket-Version: 13\r\n"));
    EXPECT_NE(std::string::npos,
              req.find("Sec-WebSocket-Key: " + key + "\r\n"));
    EXPECT_EQ("\r\n\r\n", req.substr(req.size() - 4));
}

TEST(AsteriskWsFactory, BuildUpgradeRequest_AdvertisesSipSubprotocol)
{
    auto [req, key] = AsteriskStream::build_upgrade_request("h", "/ws");
    // chan_pjsip's WS transport REQUIRES Sec-WebSocket-Protocol: sip
    // per RFC 7118 §4.
    EXPECT_NE(std::string::npos, req.find("Sec-WebSocket-Protocol: sip\r\n"));
}

TEST(AsteriskWsFactory, BuildUpgradeRequest_NoAuthHeader)
{
    auto [req, key] = AsteriskStream::build_upgrade_request("h", "/ws");
    EXPECT_EQ(std::string::npos, req.find("Authorization:"));
}

TEST(AsteriskWsFactory, BuildUpgradeRequest_GeneratesUniqueKeys)
{
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        auto [req, key] = AsteriskStream::build_upgrade_request("h", "/ws");
        EXPECT_TRUE(seen.insert(key).second);
    }
}

// ── validate_upgrade_response ────────────────────────────────────────────────

TEST(AsteriskWsFactory, ValidateUpgradeResponse_AcceptsCorrect101)
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string expected = wsframe::accept_key(key);
    const std::string headers =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Accept: " + expected + "\r\n"
        "Sec-WebSocket-Protocol: sip\r\n\r\n";
    EXPECT_TRUE(AsteriskStream::validate_upgrade_response(headers, key));
}

TEST(AsteriskWsFactory, ValidateUpgradeResponse_RejectsBadStatus)
{
    const std::string key = "x";
    EXPECT_FALSE(AsteriskStream::validate_upgrade_response(
        "HTTP/1.1 400 Bad Request\r\n\r\n", key));
}

// ── Inbound bytes → demux.on_asterisk_data ───────────────────────────────────

TEST(AsteriskWsFactory, OnInput_DispatchesToDemux)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 7;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));
    EXPECT_EQ(1u, fx.demux.active_streams());

    // Asterisk → us: a SIP REGISTER reply.
    const std::string sip_msg = "SIP/2.0 401 Unauthorized\r\n\r\n";
    const auto frame = ws_text_unmasked(sip_msg);
    ::send(fds[0], frame.data(), frame.size(), 0);
    EXPECT_EQ(0, as->handle_input(as->get_handle()));

    // The demux wrapped the payload as a DATA frame upstream.
    ASSERT_EQ(1u, fx.tun.sent.size());
    EXPECT_EQ(SipFrame::Op::DATA, fx.tun.sent[0].op);
    EXPECT_EQ(sid,                fx.tun.sent[0].stream_id);
    EXPECT_EQ(sip_msg,            fx.tun.sent[0].payload);

    delete as;
    ::close(fds[0]);
}

TEST(AsteriskWsFactory, OnInput_PingTriggersPong)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 1;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));

    const auto ping = ws_ping_unmasked();
    ::send(fds[0], ping.data(), ping.size(), 0);
    EXPECT_EQ(0, as->handle_input(as->get_handle()));

    const auto reply = drain_socket(fds[0]);
    ASSERT_GE(reply.size(), 2u);
    EXPECT_EQ(0x8A, static_cast<unsigned char>(reply[0]))
        << "FIN=1 + opcode=0xA (pong)";
    EXPECT_EQ(0x80, static_cast<unsigned char>(reply[1] & 0x80))
        << "client-side response MUST mask";

    delete as;
    ::close(fds[0]);
}

TEST(AsteriskWsFactory, OnInput_CloseFrame_NotifiesDemux)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 7;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));
    EXPECT_EQ(1u, fx.demux.active_streams());

    const auto cls = ws_close_unmasked();
    ::send(fds[0], cls.data(), cls.size(), 0);
    EXPECT_EQ(-1, as->handle_input(as->get_handle()));

    EXPECT_EQ(0u, fx.demux.active_streams())
        << "demux's on_asterisk_eof should have erased the stream";

    // The demux also sends a CLOSE frame upstream so the cloud-side
    // bridge cleans up the matching browser session.
    bool saw_close = false;
    for (const auto &f : fx.tun.sent) {
        if (f.op == SipFrame::Op::CLOSE && f.stream_id == sid) {
            saw_close = true;
            EXPECT_EQ("asterisk_closed", f.payload);
        }
    }
    EXPECT_TRUE(saw_close);

    delete as;
    ::close(fds[0]);
}

// ── Outbound: write_bytes encodes a masked WS text frame ─────────────────────

TEST(AsteriskWsFactory, WriteBytes_EncodesMaskedTextFrame)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 1;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));

    const std::string sip = "INVITE sip:b-204@society SIP/2.0\r\n\r\n";
    EXPECT_TRUE(as->write_bytes(sip));

    const auto bytes = drain_socket(fds[0]);
    ASSERT_GE(bytes.size(), 6u);
    EXPECT_EQ(0x81, static_cast<unsigned char>(bytes[0]))
        << "FIN=1, opcode=0x1 (text)";
    EXPECT_EQ(0x80, static_cast<unsigned char>(bytes[1] & 0x80))
        << "client side MUST mask";

    std::vector<std::uint8_t> buf(bytes.begin(), bytes.end());
    auto f = wsframe::decode(buf);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(0x1, f->opcode);
    EXPECT_EQ(sip, std::string(f->payload.begin(), f->payload.end()));

    delete as;
    ::close(fds[0]);
}

// ── Multi-frame + partial frame ──────────────────────────────────────────────

TEST(AsteriskWsFactory, MultipleFramesInOneRead)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 42;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));

    const auto a = ws_text_unmasked("first-reply");
    const auto b = ws_text_unmasked("second-reply");
    std::vector<std::uint8_t> blob;
    blob.insert(blob.end(), a.begin(), a.end());
    blob.insert(blob.end(), b.begin(), b.end());
    ::send(fds[0], blob.data(), blob.size(), 0);
    EXPECT_EQ(0, as->handle_input(as->get_handle()));

    ASSERT_EQ(2u, fx.tun.sent.size());
    EXPECT_EQ("first-reply",  fx.tun.sent[0].payload);
    EXPECT_EQ("second-reply", fx.tun.sent[1].payload);

    delete as;
    ::close(fds[0]);
}

TEST(AsteriskWsFactory, PartialFrameAcrossReads)
{
    DemuxFixture fx;
    constexpr std::uint32_t sid = 1;

    int fds[2]; make_socketpair(fds);
    auto *as = new AsteriskStream(fx.demux, sid, fds[1]);
    fx.factory.next = as->make_adapter();
    fx.demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, sid, "{}"));

    const auto frame = ws_text_unmasked("split-me-please");
    const std::size_t split = 5;
    ASSERT_GT(frame.size(), split);

    ::send(fds[0], frame.data(), split, 0);
    EXPECT_EQ(0, as->handle_input(as->get_handle()));
    EXPECT_EQ(0u, fx.tun.sent.size()) << "no dispatch until the frame completes";

    ::send(fds[0], frame.data() + split, frame.size() - split, 0);
    EXPECT_EQ(0, as->handle_input(as->get_handle()));
    ASSERT_EQ(1u, fx.tun.sent.size());
    EXPECT_EQ("split-me-please", fx.tun.sent[0].payload);

    delete as;
    ::close(fds[0]);
}

// ── Factory smoke: unreachable host returns nullptr ──────────────────────────

TEST(AsteriskWsFactory, FactoryUnreachableHost_ReturnsNullptr)
{
    DemuxFixture fx;
    AsteriskWsFactory fac(/*reactor=*/nullptr, fx.demux, "127.0.0.1",
                            /*port=*/1, "/ws");
    auto stream = fac.open(/*sid=*/99, "{}");
    EXPECT_EQ(nullptr, stream);
}

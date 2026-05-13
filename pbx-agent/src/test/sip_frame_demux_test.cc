#include "sip_frame_demux.hpp"
#include "sip_frame.hpp"
#include "tunnel_sink.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// In-memory tunnel: records every frame the demux sends upstream.
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

// State that outlives the FakeAsterisk instance. The demux owns the
// FakeAsterisk via unique_ptr and destroys it on close()/erase(); the
// state stays in the factory so tests can still inspect what happened.
struct AsteriskState {
  std::vector<std::string> received;
  bool closed = false;
  std::string close_reason;
  bool fail_next_send = false;
};

class FakeAsterisk : public IAsteriskStream {
public:
  explicit FakeAsterisk(AsteriskState *state) : m_state(state) {}

  bool send_bytes(const std::string &b) override {
    if (m_state->fail_next_send) return false;
    m_state->received.push_back(b);
    return true;
  }
  void close(const std::string &reason) override {
    m_state->closed = true;
    m_state->close_reason = reason;
  }

private:
  AsteriskState *m_state;
};

class FakeAsteriskFactory : public IAsteriskFactory {
public:
  std::vector<std::pair<std::uint32_t, std::string>> opens; // (sid, meta)
  std::vector<std::unique_ptr<AsteriskState>>        states;
  bool refuse_next_open = false;

  std::unique_ptr<IAsteriskStream> open(std::uint32_t sid,
                                        const std::string &meta) override {
    opens.push_back({sid, meta});
    if (refuse_next_open) {
      refuse_next_open = false;
      return nullptr;
    }
    auto state = std::make_unique<AsteriskState>();
    AsteriskState *raw = state.get();
    states.push_back(std::move(state));
    return std::make_unique<FakeAsterisk>(raw);
  }

  AsteriskState &last() { return *states.back(); }
};

} // namespace

// ── OPEN → factory.open() ─────────────────────────────────────────────────────

TEST(SipFrameDemux, OnOpenFrame_OpensLocalAsteriskSocket)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    const std::string meta = R"({"societyId":"s1","sipUsername":"u_alice"})";
    const std::string frame = SipFrame::encode(SipFrame::Op::OPEN, 7, meta);

    EXPECT_TRUE(demux.on_tunnel_bytes(frame));
    ASSERT_EQ(1u, fac.opens.size());
    EXPECT_EQ(7u, fac.opens[0].first);
    EXPECT_EQ(meta, fac.opens[0].second);
    EXPECT_EQ(1u, demux.active_streams());
}

TEST(SipFrameDemux, OnOpenFrame_FactoryRefusal_EmitsCloseUpstream)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    fac.refuse_next_open = true;
    SipFrameDemux        demux(&tun, fac);

    const std::string frame = SipFrame::encode(SipFrame::Op::OPEN, 9, "{}");
    EXPECT_TRUE(demux.on_tunnel_bytes(frame));
    EXPECT_EQ(0u, demux.active_streams());

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::CLOSE,           tun.sent[0].op);
    EXPECT_EQ(9u,                             tun.sent[0].stream_id);
    EXPECT_EQ("asterisk_unreachable",         tun.sent[0].payload);
}

TEST(SipFrameDemux, OnOpenFrame_DuplicateStreamId_IsIdempotent)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    const std::string frame = SipFrame::encode(SipFrame::Op::OPEN, 5, "{}");
    EXPECT_TRUE(demux.on_tunnel_bytes(frame));
    EXPECT_TRUE(demux.on_tunnel_bytes(frame));  // duplicate OPEN

    EXPECT_EQ(1u, fac.opens.size())
        << "factory must only be called once for the same stream-id";
    EXPECT_EQ(1u, demux.active_streams());
}

// ── DATA from cloud → Asterisk ────────────────────────────────────────────────

TEST(SipFrameDemux, OnDataFrame_PipesToAsteriskSocket)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    const std::string open  = SipFrame::encode(SipFrame::Op::OPEN, 3, "{}");
    const std::string sip_bytes =
        "REGISTER sip:society SIP/2.0\r\nVia: SIP/2.0/WSS x\r\n\r\n";
    const std::string data  = SipFrame::encode(SipFrame::Op::DATA, 3, sip_bytes);

    EXPECT_TRUE(demux.on_tunnel_bytes(open + data));

    ASSERT_EQ(1u, fac.states.size());
    ASSERT_EQ(1u, fac.states[0]->received.size());
    EXPECT_EQ(sip_bytes, fac.states[0]->received[0]);
}

TEST(SipFrameDemux, DropsUnknownStreamId)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    // DATA for stream-id 99 without a prior OPEN → silently dropped.
    const std::string data = SipFrame::encode(SipFrame::Op::DATA, 99, "ghost");
    EXPECT_TRUE(demux.on_tunnel_bytes(data));

    EXPECT_TRUE(fac.opens.empty());
    EXPECT_TRUE(tun.sent.empty()) << "must not echo back as ERR";
}

TEST(SipFrameDemux, OnDataFrame_AsteriskSendFails_ClosesStream)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 4, "{}")));
    fac.states[0]->fail_next_send = true;
    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::DATA, 4, "X")));

    EXPECT_EQ(0u, demux.active_streams());
    EXPECT_TRUE(fac.states[0]->closed);
    EXPECT_EQ("asterisk_write_failed", fac.states[0]->close_reason);
}

// ── Asterisk → cloud ──────────────────────────────────────────────────────────

TEST(SipFrameDemux, OnAsteriskData_WrapsInDataFrame)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 11, "{}")));
    tun.sent.clear();  // discard OPEN's side effects (none here, but defensive)

    const std::string asterisk_reply = "SIP/2.0 200 OK\r\nVia: x\r\n\r\n";
    demux.on_asterisk_data(11, asterisk_reply);

    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::DATA, tun.sent[0].op);
    EXPECT_EQ(11u,                 tun.sent[0].stream_id);
    EXPECT_EQ(asterisk_reply,      tun.sent[0].payload);
}

TEST(SipFrameDemux, OnAsteriskData_UnknownStream_Dropped)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    demux.on_asterisk_data(42, "stale");
    EXPECT_TRUE(tun.sent.empty());
}

TEST(SipFrameDemux, OnAsteriskClose_EmitsCloseFrame)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 13, "{}")));
    tun.sent.clear();

    demux.on_asterisk_eof(13, "channel_destroyed");
    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::CLOSE,    tun.sent[0].op);
    EXPECT_EQ(13u,                    tun.sent[0].stream_id);
    EXPECT_EQ("channel_destroyed",    tun.sent[0].payload);
    EXPECT_EQ(0u, demux.active_streams());
}

// ── Cloud-side CLOSE drops the local stream ───────────────────────────────────

TEST(SipFrameDemux, OnTunnelClose_ClosesAsteriskStream)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 8, "{}")));

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::CLOSE, 8, "browser_bye")));

    EXPECT_TRUE(fac.states[0]->closed);
    EXPECT_EQ("browser_bye", fac.states[0]->close_reason);
    EXPECT_EQ(0u, demux.active_streams());
}

// ── PING / PONG heartbeat ─────────────────────────────────────────────────────

TEST(SipFrameDemux, PingTriggersPong)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::PING, 0, "")));
    ASSERT_EQ(1u, tun.sent.size());
    EXPECT_EQ(SipFrame::Op::PONG, tun.sent[0].op);
    EXPECT_EQ(0u,                  tun.sent[0].stream_id);
}

// ── Partial frame buffering across recv() boundaries ──────────────────────────

TEST(SipFrameDemux, PartialFrameAcrossReads)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 21, "{}")));
    const std::string frame =
        SipFrame::encode(SipFrame::Op::DATA, 21, "hello-world");

    EXPECT_TRUE(demux.on_tunnel_bytes(frame.substr(0, 7)));
    EXPECT_EQ(0u, fac.states[0]->received.size())
        << "must not deliver until the full frame is buffered";
    EXPECT_TRUE(demux.on_tunnel_bytes(frame.substr(7)));

    ASSERT_EQ(1u, fac.states[0]->received.size());
    EXPECT_EQ("hello-world", fac.states[0]->received[0]);
}

// ── Tunnel disconnect closes everything ───────────────────────────────────────

TEST(SipFrameDemux, OnTunnelDisconnect_ClosesAllAsteriskStreams)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 1, "{}")));
    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 2, "{}")));
    EXPECT_TRUE(demux.on_tunnel_bytes(SipFrame::encode(SipFrame::Op::OPEN, 3, "{}")));

    demux.on_tunnel_disconnect();

    EXPECT_TRUE(fac.states[0]->closed);
    EXPECT_TRUE(fac.states[1]->closed);
    EXPECT_TRUE(fac.states[2]->closed);
    EXPECT_EQ("tunnel_lost", fac.states[0]->close_reason);
    EXPECT_EQ(0u, demux.active_streams());
}

// ── Invalid frame on the tunnel ──────────────────────────────────────────────

TEST(SipFrameDemux, OnInvalidFrame_ReturnsFalse)
{
    FakeTunnel           tun;
    FakeAsteriskFactory  fac;
    SipFrameDemux        demux(&tun, fac);

    std::string bad = SipFrame::encode(SipFrame::Op::DATA, 1, "x");
    bad[0] = 0x77;  // bogus version
    EXPECT_FALSE(demux.on_tunnel_bytes(bad));
}

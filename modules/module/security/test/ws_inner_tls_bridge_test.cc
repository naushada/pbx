// Tests for WsInnerTlsBridge — the reactor-friendly dual-mode ITransport
// adapter that layers InnerTLS over a WebSocket.
//
// Scope: just the bridge-specific behaviour (mode switching, frame
// decoding in blocking mode, queue semantics in buffered mode, leftover
// byte hand-off). The end-to-end InnerTLS round-trip is already covered
// by `innertls_test.cc` against `MockTransport`; layering it on the
// bridge would add a fragile hand-driven interleave for no extra signal.

#include "wsframe.hpp"
#include "ws_inner_tls_bridge.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

/// Tiny in-memory socket pair. Each peer's `send_raw` appends to the
/// other side's queue; `recv_raw` pops from its own. Returns -1 (EOF/
/// error) when the queue is empty so blocking-mode tests can simulate
/// peer disconnect by leaving the queue empty.
struct PairedSocket {
  std::deque<std::uint8_t> a_to_b;
  std::deque<std::uint8_t> b_to_a;

  WsInnerTlsBridge::SendRawFn a_send() {
    return [this](const void *buf, std::size_t len) -> long {
      const auto *p = static_cast<const std::uint8_t *>(buf);
      a_to_b.insert(a_to_b.end(), p, p + len);
      return static_cast<long>(len);
    };
  }
  WsInnerTlsBridge::RecvRawFn a_recv() {
    return [this](void *buf, std::size_t cap) -> long {
      if (b_to_a.empty()) return -1;
      const std::size_t n = std::min(cap, b_to_a.size());
      auto *p = static_cast<std::uint8_t *>(buf);
      for (std::size_t i = 0; i < n; ++i) {
        p[i] = b_to_a.front();
        b_to_a.pop_front();
      }
      return static_cast<long>(n);
    };
  }
  WsInnerTlsBridge::SendRawFn b_send() {
    return [this](const void *buf, std::size_t len) -> long {
      const auto *p = static_cast<const std::uint8_t *>(buf);
      b_to_a.insert(b_to_a.end(), p, p + len);
      return static_cast<long>(len);
    };
  }
};

std::vector<std::uint8_t> str_to_bytes(const std::string &s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::string bytes_to_str(const std::vector<std::uint8_t> &v) {
  return std::string(v.begin(), v.end());
}

} // namespace

// ─── blocking-mode framing ────────────────────────────────────────────────

TEST(WsInnerTlsBridge, BlockingRecv_DecodesOneBinaryFrame)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    // Peer (server-side) sends one unmasked binary frame.
    const auto frame = wsframe::encode(str_to_bytes("hello"), 0x2, false);
    sock.b_to_a.insert(sock.b_to_a.end(), frame.begin(), frame.end());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out));
    EXPECT_EQ("hello", bytes_to_str(out));
}

TEST(WsInnerTlsBridge, BlockingRecv_AutoAnswersPing)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    const auto ping   = wsframe::encode(str_to_bytes("hb"),         0x9, false);
    const auto binary = wsframe::encode(str_to_bytes("after-ping"), 0x2, false);
    sock.b_to_a.insert(sock.b_to_a.end(), ping.begin(),   ping.end());
    sock.b_to_a.insert(sock.b_to_a.end(), binary.begin(), binary.end());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out));
    EXPECT_EQ("after-ping", bytes_to_str(out))
        << "ping swallowed; next binary frame surfaces";
    EXPECT_FALSE(sock.a_to_b.empty())
        << "auto-pong landed on the wire";
}

TEST(WsInnerTlsBridge, BlockingRecv_ReturnsFalseOnCloseFrame)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    const auto cls = wsframe::close_frame();
    sock.b_to_a.insert(sock.b_to_a.end(), cls.begin(), cls.end());

    std::vector<std::uint8_t> out;
    EXPECT_FALSE(a.recv(out));
    EXPECT_TRUE(a.closed());
}

TEST(WsInnerTlsBridge, BlockingRecv_ReturnsFalseOnSocketEof)
{
    PairedSocket sock;  // queues empty → recv_raw returns -1
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    std::vector<std::uint8_t> out;
    EXPECT_FALSE(a.recv(out));
    EXPECT_TRUE(a.closed());
}

TEST(WsInnerTlsBridge, Send_EncodesAsBinaryFrame_WithMaskingPerRole)
{
    PairedSocket client_sock;
    WsInnerTlsBridge client_bridge(client_sock.a_recv(), client_sock.a_send(),
                                    /*client_mask=*/true);
    ASSERT_TRUE(client_bridge.send(str_to_bytes("payload")));

    // Pull what arrived at the peer and decode it; payload must match
    // and the frame's opcode is 0x2 (binary).
    std::vector<std::uint8_t> wire(client_sock.a_to_b.begin(),
                                    client_sock.a_to_b.end());
    auto frame = wsframe::decode(wire);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(0x2u, frame->opcode);
    EXPECT_EQ("payload", bytes_to_str(frame->payload));
}

// ─── buffered-mode queue semantics ────────────────────────────────────────

TEST(WsInnerTlsBridge, Buffered_EmptyQueue_ReturnsTrueWithEmptyData)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);
    a.switch_to_buffered();

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out))
        << "must NOT report close on an empty queue — InnerTls would "
        << "abort its loop";
    EXPECT_TRUE(out.empty());
}

TEST(WsInnerTlsBridge, Buffered_PopsOneInboundPerCall)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);
    a.switch_to_buffered();

    a.push_inbound(str_to_bytes("first"));
    a.push_inbound(str_to_bytes("second"));
    EXPECT_EQ(2u, a.buffered_inbound_count());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out));
    EXPECT_EQ("first", bytes_to_str(out));
    ASSERT_TRUE(a.recv(out));
    EXPECT_EQ("second", bytes_to_str(out));
    ASSERT_TRUE(a.recv(out));
    EXPECT_TRUE(out.empty()) << "drained queue → empty payload";
}

TEST(WsInnerTlsBridge, Buffered_MarkClosed_ReturnsFalse)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);
    a.switch_to_buffered();
    a.mark_closed();

    std::vector<std::uint8_t> out;
    EXPECT_FALSE(a.recv(out));
}

TEST(WsInnerTlsBridge, Buffered_PushEmpty_NoOp)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);
    a.switch_to_buffered();

    a.push_inbound({});  // empty
    EXPECT_EQ(0u, a.buffered_inbound_count());
}

// ─── leftover bytes the blocking-mode recv pulled past the last frame ────

TEST(WsInnerTlsBridge, LeftoverSocketBytes_HandsBackPostHandshakeBytes)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    // Stack one full frame followed by raw bytes (e.g. the leading
    // octets of an app-data frame that arrived in the same TCP segment
    // as the handshake's final record).
    const auto first    = wsframe::encode(str_to_bytes("done"), 0x2, false);
    const std::vector<std::uint8_t> trailing = {'x', 'y', 'z'};
    sock.b_to_a.insert(sock.b_to_a.end(), first.begin(),    first.end());
    sock.b_to_a.insert(sock.b_to_a.end(), trailing.begin(), trailing.end());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out));
    EXPECT_EQ("done", bytes_to_str(out));

    a.switch_to_buffered();
    auto leftover = a.leftover_socket_bytes();
    EXPECT_EQ(trailing, leftover);
    EXPECT_TRUE(a.leftover_socket_bytes().empty())
        << "idempotent — second call yields nothing";
}

TEST(WsInnerTlsBridge, LeftoverSocketBytes_EmptyWhenNoExtra)
{
    PairedSocket sock;
    WsInnerTlsBridge a(sock.a_recv(), sock.a_send(), /*client_mask=*/true);

    const auto only = wsframe::encode(str_to_bytes("p"), 0x2, false);
    sock.b_to_a.insert(sock.b_to_a.end(), only.begin(), only.end());

    std::vector<std::uint8_t> out;
    ASSERT_TRUE(a.recv(out));
    EXPECT_TRUE(a.leftover_socket_bytes().empty());
}

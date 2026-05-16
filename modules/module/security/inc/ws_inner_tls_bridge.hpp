#ifndef WS_INNER_TLS_BRIDGE_HPP
#define WS_INNER_TLS_BRIDGE_HPP

#include "innertls.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

/**
 * @file ws_inner_tls_bridge.hpp
 * @brief Reactor-friendly `IInnerTlsTransport` adapter for layering InnerTLS over
 *        a WebSocket stream owned by an ACE event handler.
 *
 * `InnerTlsClient` / `InnerTlsServer` consume an `IInnerTlsTransport` whose `recv()`
 * is expected to *block* until bytes arrive. That's a natural fit for the
 * wsdbagent's dedicated session thread, but the onprem-pbx `/agent` tunnel
 * lives on a reactor — `handle_input` is invoked when bytes are ready, and
 * a blocking recv inside it would stall the reactor.
 *
 * `WsInnerTlsBridge` resolves the impedance with two modes:
 *
 *   - **Blocking** (initial, during handshake). `recv()` synchronously
 *     reads from the underlying socket via the injected `recv_raw` callback
 *     and WS-deframes until a binary frame is available. Ping frames are
 *     auto-answered with a pong; close frames return false.
 *
 *   - **Buffered** (steady state, after handshake). The reactor's
 *     `handle_input` deframes WS itself, calls `push_inbound(...)` for each
 *     binary payload, then calls `m_inner_tls->recv()` to drain plaintext.
 *     `recv()` here pulls one queued payload per call (or returns an empty
 *     payload when the queue is drained, which `InnerTls::recv` treats as
 *     WANT_READ — the desired exit condition).
 *
 * Producer/consumer responsibilities split cleanly: the bridge owns the
 * blocking recv path during handshake, the host event handler owns frame
 * decoding during steady state. The handshake's residual unread socket
 * bytes are surfaced via `leftover_socket_bytes()` so the host can seed
 * its post-handshake decode buffer.
 *
 * Wire shape: each `send()` emits exactly one WS binary frame (opcode
 * `0x02`) carrying the encrypted record. Masking is set per the role —
 * client-mode bridges mask (RFC 6455 §5.2), server-mode bridges don't.
 *
 * Not thread-safe. Single-threaded reactor only.
 */
class WsInnerTlsBridge : public IInnerTlsTransport {
public:
  /// Best-effort blocking read from the underlying socket. Returns the
  /// number of bytes read into @p buf, 0 on clean EOF, or a negative
  /// value on error. Mirrors `ACE_(SSL_)SOCK_Stream::recv` semantics.
  using RecvRawFn = std::function<long(void *buf, std::size_t cap)>;

  /// Blocking write — must transmit all @p len bytes or fail. Returns
  /// the number actually sent; the bridge treats any short write as a
  /// fatal error (matches `send_n` semantics).
  using SendRawFn = std::function<long(const void *buf, std::size_t len)>;

  WsInnerTlsBridge(RecvRawFn recv_raw, SendRawFn send_raw, bool client_mask);

  // ── IInnerTlsTransport ─────────────────────────────────────────────────────────

  /// Encode @p data as one WS binary frame and send it through `send_raw`.
  /// Masking follows the constructor's `client_mask` flag.
  bool send(const std::vector<std::uint8_t> &data) override;

  /// Mode-dependent: blocking socket read + WS deframe during handshake;
  /// queue pop during steady state. Returns false only on a real
  /// close/error; an empty queue in buffered mode returns true with an
  /// empty @p data (matches what `InnerTls::recv` expects to exit its
  /// loop on WANT_READ).
  bool recv(std::vector<std::uint8_t> &data) override;

  // ── Mode control ──────────────────────────────────────────────────────

  /// Flip from blocking (handshake) to buffered (steady state). Idempotent.
  void switch_to_buffered();

  bool is_buffered() const { return m_buffered; }

  /// After handshake, hand the host any bytes the blocking-mode recv
  /// pulled from the socket past the last handshake record. The host's
  /// post-handshake frame-decode loop must seed its own buffer with these
  /// so traffic that arrived in the same TCP segment as the final
  /// `Finished` isn't lost. Idempotent: subsequent calls return empty.
  std::vector<std::uint8_t> leftover_socket_bytes();

  // ── Buffered-mode producer (reactor-side) ─────────────────────────────

  /// Push one decoded WS binary payload into the queue. Called by the
  /// host's `handle_input` for every steady-state inbound binary frame
  /// before invoking `InnerTls::recv()` to drain plaintext.
  void push_inbound(std::vector<std::uint8_t> data);

  /// Observability: how many queued payloads are waiting to be consumed
  /// by `InnerTls::recv()`. Mostly for tests.
  std::size_t buffered_inbound_count() const { return m_inbound.size(); }

  /// Mark the bridge closed — subsequent `recv()` returns false. Used by
  /// the host on socket EOF / peer close so a pending `InnerTls::recv`
  /// returns false (which `InnerTls` treats as a fatal transport drop).
  void mark_closed() { m_closed = true; }

  bool closed() const { return m_closed; }

private:
  /// Blocking-mode recv implementation: read from socket, deframe WS,
  /// auto-answer pings, return next binary payload. Loops until either
  /// (a) a binary/text/continuation frame arrives, or (b) the socket
  /// closes or an unrecoverable frame (close) is observed.
  bool blocking_recv(std::vector<std::uint8_t> &data);

  RecvRawFn   m_recv_raw;
  SendRawFn   m_send_raw;
  bool        m_client_mask;
  bool        m_buffered = false;
  bool        m_closed   = false;

  /// Carry-over for partial frames + post-handshake leftovers.
  std::vector<std::uint8_t>            m_socket_buf;
  /// Queue of decrypted-payload-ready binary frames in buffered mode.
  std::deque<std::vector<std::uint8_t>> m_inbound;
};

#endif // WS_INNER_TLS_BRIDGE_HPP

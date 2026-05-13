#ifndef SIP_FRAME_DEMUX_HPP
#define SIP_FRAME_DEMUX_HPP

#include "sip_frame.hpp"
#include "tunnel_sink.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

/**
 * @file sip_frame_demux.hpp
 * @brief On-prem mirror of the cloud-side `SipBridge`.
 *
 * Lives on `pbx-agent`. Receives `SipFrame` bytes from the cloud tunnel
 * and de-multiplexes them onto per-stream local sockets to Asterisk's
 * `ws://127.0.0.1:8088/ws`. Bytes flowing the other way (from Asterisk
 * back to the browser) are re-framed as `DATA` and sent up the tunnel.
 *
 * I/O is dependency-injected. Production wires `IAsteriskFactory` to
 * something that opens a real TCP socket to local Asterisk and
 * registers an `ACE_Event_Handler` to feed bytes back. Tests use the
 * in-memory `FakeAsteriskFactory` in `sip_frame_demux_test.cc`.
 *
 * See `DESIGN.md §3.2` and `TDD-PLAN.md` Layer 2 for behavioural
 * contracts.
 */

/// Abstract sink for a single per-stream connection from the agent to
/// the local Asterisk `chan_pjsip` WS endpoint.
class IAsteriskStream {
public:
  virtual ~IAsteriskStream() = default;

  /// Write raw SIP-WS bytes towards Asterisk. Returns false if the
  /// underlying socket is broken (caller should close the stream).
  virtual bool send_bytes(const std::string &sip_ws_bytes) = 0;

  /// Asterisk-bound connection is being released. The implementation
  /// is responsible for closing the underlying socket.
  virtual void close(const std::string &reason) = 0;
};

/// Factory abstraction for opening a fresh Asterisk-bound stream per
/// OPEN frame. Production: TCP-connect + WS-handshake to
/// `ws://127.0.0.1:8088/ws`. Tests: hand back an in-memory fake.
class IAsteriskFactory {
public:
  virtual ~IAsteriskFactory() = default;

  /// Open a new Asterisk-bound stream for @p stream_id with the JSON
  /// metadata received in the cloud's `OPEN` frame. Returns nullptr
  /// on connection failure (the demux logs and continues).
  virtual std::unique_ptr<IAsteriskStream> open(std::uint32_t stream_id,
                                                 const std::string &open_meta) = 0;
};

class SipFrameDemux {
public:
  /// @param tunnel  Outbound sink to the cloud (i.e. the agent end of the
  ///                mTLS tunnel). May be replaced via `set_tunnel()` on
  ///                tunnel reconnect.
  /// @param factory Factory for new per-stream Asterisk connections.
  SipFrameDemux(TunnelSink *tunnel, IAsteriskFactory &factory);
  ~SipFrameDemux() = default;

  SipFrameDemux(const SipFrameDemux &) = delete;
  SipFrameDemux &operator=(const SipFrameDemux &) = delete;

  // ── Tunnel-side entry points ───────────────────────────────────────────

  /// Bytes arrived on the tunnel. Decodes as many complete frames as the
  /// internal buffer permits. Returns false on a protocol violation
  /// (caller should drop the tunnel).
  bool on_tunnel_bytes(const std::string &bytes);

  /// Tunnel disconnected. Closes every Asterisk-bound stream and clears
  /// the map.
  void on_tunnel_disconnect();

  /// Install a fresh tunnel after reconnect. Existing streams (if any)
  /// are not migrated.
  void set_tunnel(TunnelSink *tunnel);

  // ── Asterisk-side entry points ─────────────────────────────────────────

  /// Bytes arrived on the local Asterisk socket for @p stream_id.
  /// Wraps in a `DATA` frame and forwards up the tunnel. Drops if the
  /// stream is unknown (e.g. CLOSE was already observed).
  void on_asterisk_data(std::uint32_t stream_id, const std::string &bytes);

  /// Local Asterisk socket reported EOF. Emits a `CLOSE` frame up the
  /// tunnel and forgets the stream.
  void on_asterisk_eof(std::uint32_t stream_id, const std::string &reason);

  // ── Observability ──────────────────────────────────────────────────────

  std::size_t active_streams() const { return m_streams.size(); }

private:
  void dispatch_frame(const SipFrame::Frame &f);
  void close_stream(std::uint32_t stream_id, const std::string &reason);

  TunnelSink                                                *m_tunnel;
  IAsteriskFactory                                           &m_factory;
  std::unordered_map<std::uint32_t, std::unique_ptr<IAsteriskStream>> m_streams;
  std::string                                                 m_recv_buffer;
};

#endif // SIP_FRAME_DEMUX_HPP

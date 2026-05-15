#ifndef SIP_BRIDGE_HPP
#define SIP_BRIDGE_HPP

#include "revocation_sink.hpp"
#include "sip_frame.hpp"
#include "tunnel_sink.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

/**
 * @brief Cloud-side multiplexer between many browser SIP-WS sockets and the
 *        single mTLS tunnel to pbx-agent.
 *
 * SipBridge owns the in-memory mapping `stream_id -> BrowserSink`. It is
 * driven by callbacks from `WebConnection` (browser side) and by callbacks
 * from `CloudTunnelEndpoint` (agent side). It performs **no I/O directly**
 * — production wires it to ACE sockets, tests wire it to in-memory fakes
 * (see TunnelSink / BrowserSink below).
 *
 * Threading: single-threaded. In production, every entry point runs on the
 * ACE reactor thread. No internal locking.
 *
 * Stream-id allocation is a monotonic counter that does NOT reset on tunnel
 * reconnect — the agent is free to see stream-ids it has never opened
 * before, and old stream-ids are guaranteed dead.
 *
 * See `DESIGN.md §7` for the framing protocol and `TDD-PLAN.md` Layer 1
 * for behavioural contracts.
 */

// `TunnelSink` is declared in tunnel_sink.hpp (shared with the agent's
// SipFrameDemux). Re-included via the header above.

/// Abstract sink for bytes destined to a single browser WSS. Production
/// wires this to a `WebConnection` that writes onto a per-browser
/// `ACE_SSL_SOCK_Stream`.
class BrowserSink {
public:
  virtual ~BrowserSink() = default;

  /// Forward raw SIP-WS frame bytes to the browser. Byte-faithful — the
  /// bridge does not interpret the contents.
  virtual void send_bytes(const std::string &sip_ws_bytes) = 0;

  /// The bridge is releasing this browser. Implementation should close the
  /// underlying socket and free its `WebConnection`. Called when the bridge
  /// observes a CLOSE frame from the agent OR the tunnel disconnects.
  virtual void close(const std::string &reason) = 0;
};

/// `SipBridge` is also the production `IRevocationSink`: the cloud REST
/// handlers call `revoke()` and it emits a `SUBSCRIBER_REVOKED` frame
/// down the agent tunnel.
class SipBridge : public IRevocationSink {
public:
  /// @param tunnel  The active tunnel sink. May be replaced later via
  ///                `set_tunnel()` to handle agent reconnects.
  explicit SipBridge(TunnelSink *tunnel);
  ~SipBridge() override = default;

  SipBridge(const SipBridge &) = delete;
  SipBridge &operator=(const SipBridge &) = delete;

  // ── Browser-side entry points ──────────────────────────────────────────

  /// A new browser WSS has been handed off to the bridge. Allocates and
  /// returns a stream-id, registers the sink, and emits an OPEN frame.
  /// @param browser    Sink for bytes back to this browser. Must outlive
  ///                   the stream (until the bridge calls `close()` on it
  ///                   OR the caller unregisters via `on_browser_close()`).
  /// @param open_meta  Opaque JSON payload carried in the OPEN frame
  ///                   (e.g. `{"societyId":"...","sipUsername":"..."}`).
  std::uint32_t on_browser_upgrade(BrowserSink *browser,
                                   const std::string &open_meta);

  /// Browser bytes arrived. Wrap as DATA + forward.
  void on_browser_data(std::uint32_t stream_id, const std::string &bytes);

  /// Browser closed its WSS. Emit CLOSE down the tunnel, forget the sink.
  /// The caller is responsible for the BrowserSink's lifetime afterwards.
  void on_browser_close(std::uint32_t stream_id, const std::string &reason);

  // ── Agent-side entry points ────────────────────────────────────────────

  /// Bytes arrived on the tunnel. Buffers and decodes as many complete
  /// frames as the buffer permits. Survives partial frames at the tail.
  /// Returns false if a protocol violation was detected (caller should
  /// drop the tunnel).
  bool on_tunnel_bytes(const std::string &bytes);

  /// Tunnel disconnected. Closes every browser sink and clears the map.
  /// The stream-id counter is NOT reset (DESIGN.md §7).
  void on_tunnel_disconnect();

  /// Install a fresh tunnel after reconnect. Pre-existing browser streams
  /// are not migrated (they were closed by `on_tunnel_disconnect()`).
  void set_tunnel(TunnelSink *tunnel);

  // ── Out-of-band tunnel ops (agent → cloud) ─────────────────────────────

  /// Install a handler called when the bridge sees a `PUSH_NOTIFY` frame
  /// from the agent. Payload is the JSON `{subscriberId, callerFlat,
  /// callId}` the agent originated. Production wires this to
  /// `PushSender::notify`; tests substitute a recorder.
  using PushNotifyHandler = std::function<void(const std::string &payload)>;
  void set_push_notify_handler(PushNotifyHandler h);

  /// Install a handler called when the bridge sees a `CDR_PUSH` frame
  /// from the agent. Payload is the BSON-serialised CDR document the
  /// agent finalised. Production wires this to a Mongo writer; tests
  /// substitute a recorder.
  using CdrPushHandler = std::function<void(const std::string &payload)>;
  void set_cdr_push_handler(CdrPushHandler h);

  // ── Out-of-band tunnel ops (cloud → agent) ─────────────────────────────

  /// `IRevocationSink`: a subscriber was disabled/removed on the cloud.
  /// Emits a `SUBSCRIBER_REVOKED` frame (stream-id 0, JSON payload
  /// `{societyId, sipUsername}`) down the tunnel so the agent can hang
  /// up that subscriber's live Asterisk channels. No-op when no tunnel
  /// is attached (agent disconnected) — the cloud-side session/`/sip-ws`
  /// guards still hold.
  void revoke(const std::string &society_id,
              const std::string &sip_username) override;

  // ── Observability (test surface) ───────────────────────────────────────

  /// Number of currently registered browser streams.
  std::size_t active_streams() const { return m_browsers.size(); }

  /// Last stream-id allocated by `on_browser_upgrade()`.
  std::uint32_t last_stream_id() const { return m_next_stream_id - 1; }

private:
  void dispatch_frame(const SipFrame::Frame &f);
  void close_stream(std::uint32_t stream_id, const std::string &reason);

  TunnelSink *m_tunnel;
  std::uint32_t m_next_stream_id;
  std::unordered_map<std::uint32_t, BrowserSink *> m_browsers;
  std::string m_recv_buffer; // partial-frame accumulator for tunnel side
  PushNotifyHandler m_push_handler;
  CdrPushHandler    m_cdr_handler;
};

#endif // SIP_BRIDGE_HPP

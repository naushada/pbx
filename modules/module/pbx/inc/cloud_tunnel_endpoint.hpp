#ifndef CLOUD_TUNNEL_ENDPOINT_HPP
#define CLOUD_TUNNEL_ENDPOINT_HPP

#include "iclock.hpp"
#include "sip_frame.hpp"
#include "tunnel_sink.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

class SipBridge;

/**
 * @file cloud_tunnel_endpoint.hpp
 * @brief Cloud-side accept-end of the mTLS tunnel to pbx-agent.
 *
 * Mirrors agent-side `CloudConnector`. Where `CloudConnector` actively
 * dials the cloud, `CloudTunnelEndpoint` is fed a connected transport
 * after the cloud's `/agent` WebSocket upgrade hand-off. It implements
 * `TunnelSink` so `SipBridge` (and the cloud's PushSender etc.) can
 * write frames upstream without knowing about ACE_SSL or WS framing.
 *
 * Pure logic — the concrete `IAgentTransport` is an `ACE_SSL_SOCK_Stream`
 * wrapper in production; tests substitute a recorder.
 *
 * Reactor-thread single-threaded; no internal locking.
 */

/// Single open agent connection. Symmetric to `ITransport` on the agent
/// side, just renamed to clarify direction.
class IAgentTransport {
public:
  virtual ~IAgentTransport() = default;

  /// Write raw bytes downstream towards the agent. Returns false if the
  /// underlying socket is broken (caller should treat as disconnect).
  virtual bool send(const std::string &bytes) = 0;

  /// Close the underlying socket. Idempotent.
  virtual void close() = 0;
};

// IClock is the shared wall-clock interface (also used by `PushSender`).
// See `iclock.hpp` for the contract.

class CloudTunnelEndpoint : public TunnelSink {
public:
  struct Config {
    /// Outbound frames that arrive while no agent is attached are
    /// buffered. `0` means unbounded (v1 default — society loads are
    /// small; PUSH_NOTIFY / CDR_PUSH bursts can't realistically
    /// outpace memory).
    std::size_t outbound_buffer_max = 0;

    /// SipFrame-level liveness check (DESIGN.md §6.6 / §7). After
    /// `heartbeat_interval_sec` of inbound silence from the agent the
    /// endpoint sends a connection-level `PING`; if `heartbeat_max_missed`
    /// consecutive PINGs go unanswered (no bytes received at all in
    /// between) the agent transport is dropped — `on_agent_disconnected`
    /// is invoked, the bridge is told the tunnel is lost, the outbound
    /// buffer is left intact for the next reconnect. Catches an agent
    /// that has hung without closing the WS — the WS-level keep-alive
    /// holds the transport open; this proves the peer is still answering.
    /// Symmetric to `CloudConnector::Config::heartbeat_*` on the agent.
    /// `heartbeat_interval_sec <= 0` disables the heartbeat (used by
    /// existing tests that don't drive the clock).
    ///
    /// 5 s × 3 missed = 15 s partition-detection window. Tightened
    /// from 15 s × 3 = 45 s for kiosk deployments where a security-gate
    /// intercom needs to be reachable within a few seconds of a network
    /// blip — see docs/design/operations/cloud-tunnel-liveness.md.
    int heartbeat_interval_sec = 5;
    int heartbeat_max_missed   = 3;

    /// Inner-TLS cert paths driven into every newly-attached
    /// `AgentStream`'s `setup_inner_tls()`. Heroku's router terminates
    /// the outer TLS, so this layer is the real mTLS trust boundary
    /// (mirrors `/ws/db`). Leave `cert_path` empty to skip — used by
    /// tests that drive `AgentStream` with raw frames; production
    /// requires all three paths and the agent will fail to register
    /// without them.
    struct InnerTlsConfig {
      std::string cert_path;
      std::string key_path;
      std::string ca_path;
    };
    InnerTlsConfig inner_tls;
  };

  /// @param clock  Optional. When null the heartbeat is fully disabled
  ///               regardless of `heartbeat_interval_sec` — every existing
  ///               caller using the no-clock ctors keeps working unchanged.
  CloudTunnelEndpoint();
  explicit CloudTunnelEndpoint(Config cfg);
  CloudTunnelEndpoint(Config cfg, IClock *clock);
  ~CloudTunnelEndpoint() override = default;

  CloudTunnelEndpoint(const CloudTunnelEndpoint &) = delete;
  CloudTunnelEndpoint &operator=(const CloudTunnelEndpoint &) = delete;

  /// Bind the SipBridge whose stream traffic flows through this tunnel.
  /// May be set before or after the first agent attaches.
  void attach_bridge(SipBridge *bridge);

  // ── Agent-side entry points (production: ACE event handler) ───────────

  /// The cloud just completed an `/agent` WS upgrade. Takes ownership
  /// of the transport. Any frames buffered while disconnected are
  /// flushed in order.
  void on_agent_connected(std::unique_ptr<IAgentTransport> transport);

  /// Bytes arrived on the agent transport. Forwards into the attached
  /// `SipBridge::on_tunnel_bytes`. If the bridge reports a protocol
  /// violation, drops the agent.
  void on_bytes_received(const std::string &bytes);

  /// Agent reported a fatal error / closed the WS. Closes the transport,
  /// tells the bridge the tunnel was lost, leaves the outbound buffer
  /// intact for the next reconnect.
  void on_agent_disconnected();

  // ── TunnelSink ────────────────────────────────────────────────────────

  /// Encode + send a frame to the agent. If no agent is attached, the
  /// encoded frame is buffered and flushed on the next
  /// `on_agent_connected()`. Mid-flight send failure is treated as a
  /// disconnect; the frame that triggered the failure is re-buffered.
  void send_frame(SipFrame::Op op, std::uint32_t stream_id,
                  const std::string &payload) override;

  // ── Heartbeat driver ──────────────────────────────────────────────────

  /// Drive the SipFrame-level heartbeat. Production: called by an ACE
  /// timer once per second on the reactor thread. Tests: invoked
  /// directly after advancing a `ManualClock`. No-op when no agent is
  /// attached, when no clock is wired, or when `heartbeat_interval_sec
  /// <= 0`.
  void tick();

  // ── Observability ─────────────────────────────────────────────────────

  bool        has_agent()             const { return m_transport != nullptr; }
  std::size_t buffered_frame_count()  const { return m_outbound.size(); }
  /// Heartbeat PINGs sent since the last inbound bytes. Resets to 0 on
  /// every `on_bytes_received()` and on every `on_agent_connected()`.
  /// Reaches `Config::heartbeat_max_missed` only when the agent has
  /// gone silent — the next `tick()` then drops the transport.
  int         pings_outstanding()     const { return m_pings_outstanding; }
  /// `WebConnection`'s `/agent` upgrade reads this to drive each new
  /// `AgentStream::setup_inner_tls(...)` before attaching.
  const Config::InnerTlsConfig &inner_tls_config() const {
    return m_cfg.inner_tls;
  }

private:
  void mark_disconnected();
  void flush_outbound();
  /// Called from `tick()` while connected: sends a heartbeat PING once
  /// per `heartbeat_interval_sec` of inbound silence, and drops the
  /// agent once `heartbeat_max_missed` PINGs have gone unanswered.
  void maybe_heartbeat();

  Config                          m_cfg;
  IClock                         *m_clock   = nullptr;
  SipBridge                      *m_bridge  = nullptr;
  std::unique_ptr<IAgentTransport> m_transport;
  std::deque<std::string>          m_outbound;
  std::int64_t  m_last_heartbeat_unix = 0; // when the last PING went out
  int           m_pings_outstanding   = 0; // PINGs sent with no inbound since
};

#endif // CLOUD_TUNNEL_ENDPOINT_HPP

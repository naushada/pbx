#ifndef CLOUD_TUNNEL_ENDPOINT_HPP
#define CLOUD_TUNNEL_ENDPOINT_HPP

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

class CloudTunnelEndpoint : public TunnelSink {
public:
  struct Config {
    /// Outbound frames that arrive while no agent is attached are
    /// buffered. `0` means unbounded (v1 default — society loads are
    /// small; PUSH_NOTIFY / CDR_PUSH bursts can't realistically
    /// outpace memory).
    std::size_t outbound_buffer_max = 0;
  };

  CloudTunnelEndpoint();
  explicit CloudTunnelEndpoint(Config cfg);
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

  // ── Observability ─────────────────────────────────────────────────────

  bool        has_agent()             const { return m_transport != nullptr; }
  std::size_t buffered_frame_count()  const { return m_outbound.size(); }

private:
  void mark_disconnected();
  void flush_outbound();

  Config                          m_cfg;
  SipBridge                      *m_bridge = nullptr;
  std::unique_ptr<IAgentTransport> m_transport;
  std::deque<std::string>          m_outbound;
};

#endif // CLOUD_TUNNEL_ENDPOINT_HPP

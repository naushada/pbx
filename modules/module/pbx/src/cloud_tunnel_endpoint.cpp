#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"

#include <utility>

CloudTunnelEndpoint::CloudTunnelEndpoint() : m_cfg{} {}
CloudTunnelEndpoint::CloudTunnelEndpoint(Config cfg) : m_cfg(cfg) {}
CloudTunnelEndpoint::CloudTunnelEndpoint(Config cfg, IClock *clock)
    : m_cfg(cfg), m_clock(clock) {}

void CloudTunnelEndpoint::attach_bridge(SipBridge *bridge) { m_bridge = bridge; }

void CloudTunnelEndpoint::on_agent_connected(
    std::unique_ptr<IAgentTransport> transport) {
  if (!transport) return;

  // If a previous agent is still hanging on (shouldn't, but be defensive),
  // tear it down first so the bridge sees a clean disconnect.
  if (m_transport) mark_disconnected();

  m_transport = std::move(transport);
  // Reset heartbeat: the next PING is due `heartbeat_interval_sec` from now.
  m_pings_outstanding = 0;
  if (m_clock) m_last_heartbeat_unix = m_clock->now_unix();
  flush_outbound();
}

void CloudTunnelEndpoint::on_bytes_received(const std::string &bytes) {
  // Any inbound bytes are proof the agent is alive — clear the heartbeat
  // miss counter regardless of what the bytes decode to (a PONG on an
  // idle tunnel, DATA / PING during an active call: all prove liveness).
  m_pings_outstanding = 0;

  if (!m_bridge) return;
  if (!m_bridge->on_tunnel_bytes(bytes)) {
    // Protocol violation from the agent. Drop the tunnel cleanly so the
    // agent's CloudConnector reconnects on its own backoff.
    mark_disconnected();
  }
}

void CloudTunnelEndpoint::on_agent_disconnected() {
  if (m_transport) mark_disconnected();
}

void CloudTunnelEndpoint::send_frame(SipFrame::Op op, std::uint32_t stream_id,
                                      const std::string &payload) {
  const std::string bytes = SipFrame::encode(op, stream_id, payload);

  if (m_transport && m_transport->send(bytes)) return;

  // Either no agent, or mid-flight send failed.
  if (m_transport) {
    mark_disconnected();
  }

  if (m_cfg.outbound_buffer_max == 0 ||
      m_outbound.size() < m_cfg.outbound_buffer_max) {
    m_outbound.push_back(bytes);
  }
  // Else: silently drop. v2 may surface an error to the producer.
}

void CloudTunnelEndpoint::mark_disconnected() {
  if (m_transport) {
    m_transport->close();
    m_transport.reset();
  }
  // Tell the bridge so it can clean up per-stream state. The bridge's
  // stream-id counter does NOT reset, matching the agent-side contract
  // (see DESIGN.md §7).
  if (m_bridge) m_bridge->on_tunnel_disconnect();
}

void CloudTunnelEndpoint::flush_outbound() {
  while (!m_outbound.empty() && m_transport) {
    if (!m_transport->send(m_outbound.front())) {
      mark_disconnected();
      return;
    }
    m_outbound.pop_front();
  }
}

void CloudTunnelEndpoint::tick() {
  if (m_transport) maybe_heartbeat();
  // No agent attached → nothing to drive. The reconnect path lives at the
  // agent end; the cloud just waits for the next /agent WS upgrade.
}

void CloudTunnelEndpoint::maybe_heartbeat() {
  if (!m_clock) return;                              // no clock → disabled
  if (m_cfg.heartbeat_interval_sec <= 0) return;     // config-disabled

  const std::int64_t now = m_clock->now_unix();
  if (now - m_last_heartbeat_unix < m_cfg.heartbeat_interval_sec)
    return; // not yet time for the next heartbeat

  if (m_pings_outstanding >= m_cfg.heartbeat_max_missed) {
    // We've sent `heartbeat_max_missed` PINGs and gotten zero bytes back.
    // The transport still looks healthy but the peer is unresponsive —
    // drop it. The agent's CloudConnector will reconnect on its backoff.
    mark_disconnected();
    return;
  }

  // Connection-level heartbeat: stream-id 0, empty payload (DESIGN.md §7).
  // send_frame() handles a mid-flight send failure by marking us
  // disconnected, so a dead socket is caught here too.
  send_frame(SipFrame::Op::PING, 0, {});
  if (m_transport) {
    // Still connected after the send — count this PING as outstanding
    // until on_bytes_received() clears it.
    ++m_pings_outstanding;
    m_last_heartbeat_unix = now;
  }
}

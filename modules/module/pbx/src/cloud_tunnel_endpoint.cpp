#include "cloud_tunnel_endpoint.hpp"
#include "sip_bridge.hpp"

#include <utility>

CloudTunnelEndpoint::CloudTunnelEndpoint() : m_cfg{} {}
CloudTunnelEndpoint::CloudTunnelEndpoint(Config cfg) : m_cfg(cfg) {}

void CloudTunnelEndpoint::attach_bridge(SipBridge *bridge) { m_bridge = bridge; }

void CloudTunnelEndpoint::on_agent_connected(
    std::unique_ptr<IAgentTransport> transport) {
  if (!transport) return;

  // If a previous agent is still hanging on (shouldn't, but be defensive),
  // tear it down first so the bridge sees a clean disconnect.
  if (m_transport) mark_disconnected();

  m_transport = std::move(transport);
  flush_outbound();
}

void CloudTunnelEndpoint::on_bytes_received(const std::string &bytes) {
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

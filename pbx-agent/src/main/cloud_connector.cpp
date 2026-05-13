#include "cloud_connector.hpp"
#include "sip_frame_demux.hpp"

#include <algorithm>
#include <utility>

CloudConnector::CloudConnector(Config cfg, ITransportFactory &factory,
                                IClock &clock)
    : m_cfg(std::move(cfg)), m_factory(factory), m_clock(clock) {}

void CloudConnector::attach_demux(SipFrameDemux *demux) { m_demux = demux; }

bool CloudConnector::connected() const { return m_transport != nullptr; }

void CloudConnector::send_frame(SipFrame::Op op, std::uint32_t stream_id,
                                 const std::string &payload) {
  const std::string bytes = SipFrame::encode(op, stream_id, payload);

  if (m_transport && m_transport->send(bytes)) return;

  // Transport is gone or refused — buffer + arm reconnect. If the
  // transport-send failed mid-flight, treat as a disconnect.
  if (m_transport) {
    mark_disconnected();
  }

  if (m_cfg.outbound_buffer_max == 0 ||
      m_outbound.size() < m_cfg.outbound_buffer_max) {
    m_outbound.push_back(bytes);
  }
  // Else: silently drop. Bounded buffer for v2; v1 uses unbounded.
}

void CloudConnector::tick() {
  if (m_transport) return; // already connected; nothing to do

  if (m_clock.now_unix() < m_next_reconnect_at) return; // still backing off

  attempt_connect();
}

void CloudConnector::on_bytes_received(const std::string &bytes) {
  if (!m_demux) return;
  if (!m_demux->on_tunnel_bytes(bytes)) {
    // Protocol violation — drop the tunnel so the cloud sees a clean reset.
    mark_disconnected();
  }
}

void CloudConnector::on_transport_lost() {
  if (m_transport) mark_disconnected();
}

void CloudConnector::attempt_connect() {
  auto t = m_factory.create_connected(m_cfg.host, m_cfg.port,
                                       m_cfg.client_cert_path,
                                       m_cfg.client_key_path,
                                       m_cfg.server_ca_path);
  if (!t) {
    // Connect failed — back off.
    ++m_reconnect_attempts;
    if (m_current_backoff_sec == 0)
      m_current_backoff_sec = m_cfg.initial_backoff_sec;
    else
      m_current_backoff_sec = std::min(m_current_backoff_sec * 2,
                                        m_cfg.max_backoff_sec);
    m_next_reconnect_at = m_clock.now_unix() + m_current_backoff_sec;
    return;
  }

  // Success: install transport, reset backoff, flush buffered frames.
  m_transport            = std::move(t);
  m_reconnect_attempts   = 0;
  m_current_backoff_sec  = 0;
  m_next_reconnect_at    = 0;
  flush_outbound();
}

void CloudConnector::mark_disconnected() {
  if (m_transport) {
    m_transport->close();
    m_transport.reset();
  }
  // Tell the demux any in-flight streams are gone so it can clean up.
  if (m_demux) m_demux->on_tunnel_disconnect();

  // Arm the backoff timer. First post-disconnect retry happens on the
  // next tick (next_reconnect_at = 0); subsequent failures grow the
  // backoff via attempt_connect().
  m_current_backoff_sec = 0;
  m_next_reconnect_at   = 0;
}

void CloudConnector::flush_outbound() {
  while (!m_outbound.empty() && m_transport) {
    if (!m_transport->send(m_outbound.front())) {
      // Send failed mid-flush — disconnect, leave the remaining queued.
      mark_disconnected();
      return;
    }
    m_outbound.pop_front();
  }
}

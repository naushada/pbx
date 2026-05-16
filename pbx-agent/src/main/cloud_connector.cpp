#include "cloud_connector.hpp"
#include "sip_frame_demux.hpp"

#include <algorithm>
#include <utility>

CloudConnector::CloudConnector(Config cfg, ITransportFactory &factory,
                                IClock &clock)
    : m_cfg(std::move(cfg)), m_factory(factory), m_clock(clock) {}

void CloudConnector::attach_demux(SipFrameDemux *demux) { m_demux = demux; }

void CloudConnector::set_on_connected(OnConnectedHandler h) {
  m_on_connected = std::move(h);
}

bool CloudConnector::connected() const {
  // A pending disconnect means a callback has already observed a
  // fatal transport error — the actual reset() runs on the next
  // tick(). Until then, callers see us as disconnected so they don't
  // queue more writes through a transport that's about to die.
  return m_transport != nullptr && !m_disconnect_pending;
}

void CloudConnector::send_frame(SipFrame::Op op, std::uint32_t stream_id,
                                 const std::string &payload) {
  const std::string bytes = SipFrame::encode(op, stream_id, payload);

  if (m_transport && !m_disconnect_pending && m_transport->send(bytes))
    return;

  // Transport is gone, marked-for-teardown, or refused. If we just
  // observed a fresh send failure (transport still alive, no prior
  // pending flag), request a deferred disconnect — NOT
  // mark_disconnected(): we are inside m_transport->send()'s call
  // frame, so an immediate reset() would free the very object whose
  // method is still on the stack (SIGBUS). tick() runs the actual
  // teardown on the reactor thread, outside any callback.
  if (m_transport && !m_disconnect_pending) {
    request_disconnect();
  }

  if (m_cfg.outbound_buffer_max == 0 ||
      m_outbound.size() < m_cfg.outbound_buffer_max) {
    m_outbound.push_back(bytes);
  }
  // Else: silently drop. Bounded buffer for v2; v1 uses unbounded.
}

void CloudConnector::tick() {
  // Drain a deferred disconnect FIRST — any code path that ran since
  // the last tick may have set m_disconnect_pending from inside a
  // transport callback. mark_disconnected() is safe here because tick
  // runs on the reactor thread outside any inner call frame.
  if (m_disconnect_pending) {
    m_disconnect_pending = false;
    if (m_transport) mark_disconnected();
  }

  if (m_transport) {
    // Connected: the only periodic work is the SipFrame-level heartbeat.
    maybe_heartbeat();
    return;
  }

  if (m_clock.now_unix() < m_next_reconnect_at) return; // still backing off

  attempt_connect();
}

void CloudConnector::on_bytes_received(const std::string &bytes) {
  // Any inbound bytes are proof the peer is alive — clear the heartbeat
  // miss counter regardless of what the bytes decode to (a PONG during an
  // idle tunnel, DATA during an active call: both prove liveness).
  m_pings_outstanding = 0;

  if (!m_demux) return;
  if (!m_demux->on_tunnel_bytes(bytes)) {
    // Protocol violation — drop the tunnel so the cloud sees a clean
    // reset. Deferred: this path may be reached from the dispatch task
    // thread (or recursively from inside drain_frames), so reset() now
    // would create the same UAF as the outbound case.
    request_disconnect();
  }
}

void CloudConnector::on_transport_lost() {
  // Called from AceSslTransport::notify_disconnect_once — i.e. from
  // INSIDE the transport's own method (handle_input, send, etc.).
  // Reset now would free `this` mid-call. Defer to tick().
  if (m_transport) request_disconnect();
}

void CloudConnector::request_disconnect() {
  // Idempotent — multiple producers (handle_input EOF, send-failure,
  // demux-violation) can all set this; tick() processes once.
  m_disconnect_pending = true;
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
  // Arm the heartbeat clock so the first PING is one full interval out, not
  // immediately on the next tick.
  m_last_heartbeat_unix  = m_clock.now_unix();
  m_pings_outstanding    = 0;
  flush_outbound();

  // Production: re-publishes the agent's view of every PJSIP endpoint
  // so the cloud presence cache resyncs (the cache may have gone stale
  // mid-disconnect — see AriClient::publish_register_snapshot).
  // The handler is allowed to call back into send_frame() synchronously
  // — the transport is fully installed before this fires.
  if (m_on_connected) m_on_connected();
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

  // Heartbeat state is meaningless without a transport; the next
  // successful connect re-arms it.
  m_pings_outstanding = 0;

  // The disconnect is fully realized — clear the pending flag so a
  // subsequent request_disconnect() from a future transport's
  // callback isn't masked.
  m_disconnect_pending = false;
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

void CloudConnector::maybe_heartbeat() {
  if (m_cfg.heartbeat_interval_sec <= 0) return; // heartbeat disabled

  const std::int64_t now = m_clock.now_unix();
  if (now - m_last_heartbeat_unix < m_cfg.heartbeat_interval_sec)
    return; // not yet time for the next heartbeat

  if (m_pings_outstanding >= m_cfg.heartbeat_max_missed) {
    // We've sent heartbeat_max_missed PINGs and gotten zero bytes back.
    // The transport still looks healthy but the peer is unresponsive —
    // drop it so the reconnect path can re-establish a live tunnel.
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

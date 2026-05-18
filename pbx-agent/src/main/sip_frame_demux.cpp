#include "sip_frame_demux.hpp"

#include <ace/Log_Msg.h>

SipFrameDemux::SipFrameDemux(TunnelSink *tunnel, IAsteriskFactory &factory)
    : m_tunnel(tunnel), m_factory(factory) {}

bool SipFrameDemux::on_tunnel_bytes(const std::string &bytes) {
  m_recv_buffer.append(bytes);

  for (;;) {
    const auto r = SipFrame::decode(m_recv_buffer);
    if (r.status == SipFrame::Status::NeedMore)
      return true;
    if (r.status == SipFrame::Status::Invalid) {
      m_recv_buffer.clear();
      return false;
    }
    dispatch_frame(r.frame);
    m_recv_buffer.erase(0, r.consumed);
  }
}

void SipFrameDemux::on_tunnel_disconnect() {
  for (auto &kv : m_streams)
    if (kv.second) kv.second->close("tunnel_lost");
  m_streams.clear();
  m_recv_buffer.clear();
  m_tunnel = nullptr;
}

void SipFrameDemux::set_tunnel(TunnelSink *tunnel) {
  m_tunnel = tunnel;
  m_recv_buffer.clear();
}

void SipFrameDemux::set_society_bootstrap_handler(
    SocietyBootstrapHandler h) {
  m_society_bootstrap_handler = std::move(h);
}

void SipFrameDemux::set_subscriber_revoked_handler(
    SubscriberRevokedHandler h) {
  m_revoked_handler = std::move(h);
}

void SipFrameDemux::on_asterisk_data(std::uint32_t stream_id,
                                      const std::string &bytes) {
  // Hop 3 of the reverse trace (task #36): demux received bytes from
  // an AsteriskStream, about to ship them upstream via the tunnel.
  const bool known    = m_streams.find(stream_id) != m_streams.end();
  const bool have_t   = m_tunnel != nullptr;
  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [SipFrameDemux:%t] on_asterisk_data sid=%u "
                      "bytes=%u known=%d tunnel=%d\n"),
             stream_id, static_cast<unsigned>(bytes.size()),
             known ? 1 : 0, have_t ? 1 : 0));
  if (!known) return; // Local socket bytes for a stream we already closed.
  if (m_tunnel)
    m_tunnel->send_frame(SipFrame::Op::DATA, stream_id, bytes);
}

void SipFrameDemux::on_asterisk_eof(std::uint32_t stream_id,
                                     const std::string &reason) {
  if (m_streams.erase(stream_id) == 0)
    return;
  if (m_tunnel)
    m_tunnel->send_frame(SipFrame::Op::CLOSE, stream_id, reason);
}

void SipFrameDemux::dispatch_frame(const SipFrame::Frame &f) {
  using SipFrame::Op;
  switch (f.op) {
  case Op::OPEN: {
    // Idempotent: if stream-id already known, leave it alone.
    if (m_streams.find(f.stream_id) != m_streams.end())
      return;
    auto stream = m_factory.open(f.stream_id, f.payload);
    if (!stream) {
      // Failed to dial local Asterisk. Notify cloud so the browser
      // doesn't hang waiting on a stream that will never exist.
      if (m_tunnel)
        m_tunnel->send_frame(Op::CLOSE, f.stream_id, "asterisk_unreachable");
      return;
    }
    m_streams.emplace(f.stream_id, std::move(stream));
    return;
  }
  case Op::DATA: {
    auto it = m_streams.find(f.stream_id);
    if (it == m_streams.end()) {
      // Cloud sent DATA for an unknown stream — log + drop. Don't echo
      // ERR back; the cloud's SipBridge already cleaned up its side.
      return;
    }
    if (it->second) {
      if (!it->second->send_bytes(f.payload)) {
        // Stream is broken; treat as EOF.
        close_stream(f.stream_id, "asterisk_write_failed");
      }
    }
    return;
  }
  case Op::CLOSE: {
    close_stream(f.stream_id, f.payload);
    return;
  }
  case Op::PING: {
    if (m_tunnel)
      m_tunnel->send_frame(Op::PONG, f.stream_id, {});
    return;
  }
  case Op::SUBSCRIBER_REVOKED:
    // Cloud-originated control op: an admin disabled/removed a subscriber.
    // Not tied to a stream-id — hand the JSON `{societyId, sipUsername}`
    // payload to the handler (production: ARI live-call teardown).
    if (m_revoked_handler) m_revoked_handler(f.payload);
    return;
  case Op::SOCIETY_BOOTSTRAP:
    // Cloud-originated control op (response to our AGENT_HELLO): the
    // canonical `sipRealm` for this society. Production: update
    // PjsipProvisioner + SubscriberWatcher.resync().
    if (m_society_bootstrap_handler) m_society_bootstrap_handler(f.payload);
    return;
  case Op::PONG:
  case Op::ERR:
  case Op::PUSH_NOTIFY:     // PUSH_NOTIFY is agent → cloud only; ignore if seen here.
  case Op::CDR_PUSH:        // CDR_PUSH is also agent → cloud only.
  case Op::REGISTER_STATE:  // REGISTER_STATE is also agent → cloud only.
  case Op::AGENT_HELLO:     // AGENT_HELLO is also agent → cloud only; ignore on echo.
  case Op::ASTERISK_STATUS: // ASTERISK_STATUS is also agent → cloud only; ignore on echo.
    return;
  }
}

void SipFrameDemux::close_stream(std::uint32_t stream_id,
                                  const std::string &reason) {
  auto it = m_streams.find(stream_id);
  if (it == m_streams.end()) return;
  if (it->second) it->second->close(reason);
  m_streams.erase(it);
}

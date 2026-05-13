#include "sip_bridge.hpp"

SipBridge::SipBridge(TunnelSink *tunnel)
    : m_tunnel(tunnel), m_next_stream_id(1) {}

std::uint32_t SipBridge::on_browser_upgrade(BrowserSink *browser,
                                            const std::string &open_meta) {
  const std::uint32_t sid = m_next_stream_id++;
  m_browsers.emplace(sid, browser);
  if (m_tunnel)
    m_tunnel->send_frame(SipFrame::Op::OPEN, sid, open_meta);
  return sid;
}

void SipBridge::on_browser_data(std::uint32_t stream_id,
                                 const std::string &bytes) {
  // Browser sent bytes for a stream we no longer know about: drop quietly.
  if (m_browsers.find(stream_id) == m_browsers.end())
    return;
  if (m_tunnel)
    m_tunnel->send_frame(SipFrame::Op::DATA, stream_id, bytes);
}

void SipBridge::on_browser_close(std::uint32_t stream_id,
                                  const std::string &reason) {
  if (m_browsers.erase(stream_id) == 0)
    return;
  if (m_tunnel)
    m_tunnel->send_frame(SipFrame::Op::CLOSE, stream_id, reason);
}

bool SipBridge::on_tunnel_bytes(const std::string &bytes) {
  m_recv_buffer.append(bytes);

  for (;;) {
    const auto r = SipFrame::decode(m_recv_buffer);
    if (r.status == SipFrame::Status::NeedMore)
      return true;

    if (r.status == SipFrame::Status::Invalid) {
      // Protocol violation. Discard the bad bytes and signal upstream.
      m_recv_buffer.clear();
      return false;
    }

    // Status::Ok
    dispatch_frame(r.frame);
    // Advance past the consumed bytes. Use erase() to keep any tail.
    m_recv_buffer.erase(0, r.consumed);
  }
}

void SipBridge::on_tunnel_disconnect() {
  for (auto &kv : m_browsers)
    if (kv.second)
      kv.second->close("tunnel_lost");
  m_browsers.clear();
  m_recv_buffer.clear();
  m_tunnel = nullptr;
}

void SipBridge::set_tunnel(TunnelSink *tunnel) {
  m_tunnel = tunnel;
  // m_next_stream_id is intentionally NOT reset; old ids stay dead.
  m_recv_buffer.clear();
}

void SipBridge::dispatch_frame(const SipFrame::Frame &f) {
  using SipFrame::Op;
  switch (f.op) {
  case Op::DATA: {
    auto it = m_browsers.find(f.stream_id);
    if (it != m_browsers.end() && it->second)
      it->second->send_bytes(f.payload);
    // Unknown stream-id is logged-and-dropped; do not echo back as ERR.
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
  case Op::PONG:
  case Op::ERR:
    // PONG: heartbeat ack consumed by liveness tracking (not yet wired).
    // ERR : tunnel will be torn down by the peer; passive ack here.
    return;
  case Op::OPEN:
    // OPEN is cloud→agent only. Agent must never originate it. Drop.
    return;
  case Op::PUSH_NOTIFY:
    if (m_push_handler) m_push_handler(f.payload);
    return;
  case Op::CDR_PUSH:
    if (m_cdr_handler)  m_cdr_handler(f.payload);
    return;
  }
}

void SipBridge::set_push_notify_handler(PushNotifyHandler h) {
  m_push_handler = std::move(h);
}

void SipBridge::set_cdr_push_handler(CdrPushHandler h) {
  m_cdr_handler = std::move(h);
}

void SipBridge::close_stream(std::uint32_t stream_id,
                              const std::string &reason) {
  auto it = m_browsers.find(stream_id);
  if (it == m_browsers.end())
    return;
  if (it->second)
    it->second->close(reason);
  m_browsers.erase(it);
}

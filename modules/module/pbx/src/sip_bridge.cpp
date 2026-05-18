#include "sip_bridge.hpp"

#include "json.hpp"

#include "ace/Log_Msg.h"

using json = nlohmann::json;

SipBridge::SipBridge(TunnelSink *tunnel)
    : m_tunnel(tunnel), m_next_stream_id(1) {}

std::uint32_t SipBridge::on_browser_upgrade(BrowserSink *browser,
                                            const std::string &open_meta) {
  const std::uint32_t sid = m_next_stream_id++;
  m_browsers.emplace(sid, browser);
  // Diagnostic: surface whether the OPEN frame is going out NOW or being
  // buffered against a missing tunnel. The "no tunnel" path is the silent
  // failure mode — the browser sees a successful WS upgrade but its first
  // SIP REGISTER never produces a response.
  ACE_DEBUG((LM_INFO,
             ACE_TEXT("%D [SipBridge:%t] %M %N:%l on_browser_upgrade sid=%u "
                      "tunnel_attached=%d open_meta=%s\n"),
             sid, m_tunnel ? 1 : 0, open_meta.c_str()));
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
  // Drop the agent-reported Asterisk flag — the agent is gone, so its
  // last "true" no longer reflects reality. Chip renders disconnected
  // until the next ASTERISK_STATUS arrives from the next agent.
  m_asterisk_connected = false;
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
    // Hop 5 of the reverse trace (task #36): cloud dispatches DATA
    // frame from agent to per-sid browser.
    const bool found     = it != m_browsers.end();
    const bool has_sink  = found && it->second != nullptr;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [SipBridge:%t] DATA sid=%u bytes=%u "
                        "found=%d sink=%d\n"),
               f.stream_id, static_cast<unsigned>(f.payload.size()),
               found ? 1 : 0, has_sink ? 1 : 0));
    if (has_sink)
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
    // The cloud answers PINGs (case PING above) but never originates them —
    // the agent owns the tunnel heartbeat and its liveness tracking (see
    // CloudConnector). A PONG arriving here is unexpected in v1; drop it
    // rather than treat it as a protocol error.
  case Op::ERR:
    // ERR : tunnel will be torn down by the peer; passive ack here.
    return;
  case Op::OPEN:
    // OPEN is cloud→agent only. Agent must never originate it. Drop.
    return;
  case Op::SUBSCRIBER_REVOKED:
    // SUBSCRIBER_REVOKED is cloud→agent only — the bridge *emits* it (see
    // revoke()), it never receives one. Drop if the agent echoes it back.
    return;
  case Op::PUSH_NOTIFY:
    if (m_push_handler) m_push_handler(f.payload);
    return;
  case Op::CDR_PUSH:
    if (m_cdr_handler)  m_cdr_handler(f.payload);
    return;
  case Op::REGISTER_STATE:
    if (m_register_state_handler) m_register_state_handler(f.payload);
    return;
  case Op::AGENT_HELLO:
    if (m_agent_hello_handler) m_agent_hello_handler(f.payload);
    return;
  case Op::SOCIETY_BOOTSTRAP:
    // SOCIETY_BOOTSTRAP is cloud→agent only — the bridge emits it (see
    // bootstrap_society()), it never receives one. Drop if the agent
    // echoes it back.
    return;
  case Op::ASTERISK_STATUS: {
    // Update the cached flag BEFORE invoking the handler, so a
    // production handler that reads `asterisk_connected()` sees the
    // fresh value. Bad JSON leaves the flag untouched — the chip
    // continues to show the last good signal until the next probe.
    try {
      const auto j = json::parse(f.payload);
      if (j.contains("connected") && j["connected"].is_boolean())
        m_asterisk_connected = j["connected"].get<bool>();
    } catch (...) { /* swallow */ }
    if (m_asterisk_status_handler) m_asterisk_status_handler(f.payload);
    return;
  }
  }
}

void SipBridge::set_push_notify_handler(PushNotifyHandler h) {
  m_push_handler = std::move(h);
}

void SipBridge::set_cdr_push_handler(CdrPushHandler h) {
  m_cdr_handler = std::move(h);
}

void SipBridge::set_register_state_handler(RegisterStateHandler h) {
  m_register_state_handler = std::move(h);
}

void SipBridge::set_agent_hello_handler(AgentHelloHandler h) {
  m_agent_hello_handler = std::move(h);
}

void SipBridge::set_asterisk_status_handler(AsteriskStatusHandler h) {
  m_asterisk_status_handler = std::move(h);
}

void SipBridge::bootstrap_society(const std::string &society_id,
                                   const std::string &sip_realm) {
  if (!m_tunnel) return;
  const json payload = {{"societyId", society_id},
                        {"sipRealm",  sip_realm}};
  // stream-id 0 — SOCIETY_BOOTSTRAP is tunnel-wide, not tied to any
  // browser stream. Emitted in response to AGENT_HELLO; carries the
  // canonical sipRealm so the agent's PjsipProvisioner stops
  // depending on a `--sip-realm` CLI flag for correctness.
  m_tunnel->send_frame(SipFrame::Op::SOCIETY_BOOTSTRAP, 0, payload.dump());
}

void SipBridge::revoke(const std::string &society_id,
                       const std::string &sip_username) {
  // Best-effort: if the agent tunnel is down there is nothing to notify —
  // the cloud-side session-row deletion + `/sip-ws` status gate still
  // keep the subscriber out; this frame only cuts a *currently live* call.
  if (!m_tunnel) return;
  const json payload = {{"societyId", society_id},
                        {"sipUsername", sip_username}};
  // stream-id 0 — SUBSCRIBER_REVOKED is a tunnel-wide control frame, not
  // tied to any one browser stream.
  m_tunnel->send_frame(SipFrame::Op::SUBSCRIBER_REVOKED, 0, payload.dump());
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

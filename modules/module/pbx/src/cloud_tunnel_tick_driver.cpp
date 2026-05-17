#include "cloud_tunnel_tick_driver.hpp"
#include "cloud_tunnel_endpoint.hpp"

#include <ace/Reactor.h>
#include <ace/Time_Value.h>
#include <ace/Log_Msg.h>

CloudTunnelTickDriver::CloudTunnelTickDriver(CloudTunnelEndpoint &cte,
                                              int interval_sec)
    : m_cte(cte), m_interval_sec(interval_sec) {}

CloudTunnelTickDriver::~CloudTunnelTickDriver() {
  // Defensive: if the owner forgot to cancel, do it here. Safe even
  // when never scheduled (m_timer_id stays -1).
  cancel_with_reactor();
}

int CloudTunnelTickDriver::register_with_reactor(ACE_Reactor *r) {
  if (!r) return -1;
  m_reactor = r;
  this->reactor(r);

  const ACE_Time_Value interval(m_interval_sec);
  // Initial delay == interval — no synchronous tick at startup.
  m_timer_id = r->schedule_timer(this, nullptr, interval, interval);
  if (m_timer_id == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [CloudTunnelTickDriver] schedule_timer "
                        "failed for interval=%ds\n"),
               m_interval_sec));
    return -1;
  }
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [CloudTunnelTickDriver] scheduled tick "
                      "every %ds (timer_id=%ld)\n"),
             m_interval_sec, m_timer_id));
  return 0;
}

int CloudTunnelTickDriver::cancel_with_reactor() {
  if (m_timer_id == -1) return 0; // nothing to cancel
  if (m_reactor) m_reactor->cancel_timer(m_timer_id);
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [CloudTunnelTickDriver] cancelled "
                      "(timer_id=%ld)\n"),
             m_timer_id));
  m_timer_id = -1;
  return 0;
}

int CloudTunnelTickDriver::handle_timeout(const ACE_Time_Value & /*tv*/,
                                          const void * /*act*/) {
  m_cte.tick();
  return 0; // keep the recurring timer alive
}

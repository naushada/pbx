#ifndef REVOCATION_SINK_HPP
#define REVOCATION_SINK_HPP

#include <string>

/**
 * @file revocation_sink.hpp
 * @brief Contract for cutting a subscriber's live tunnel presence.
 *
 * When the cloud disables or removes a subscriber, something must reach
 * the on-prem agent so it can tear down that subscriber's live Asterisk
 * channels — a still-open browser tab keeps a working `/sip-ws` socket
 * (and SIP registration) until it naturally drops, otherwise.
 *
 * `IRevocationSink` is that seam: a one-method interface the cloud REST
 * handlers (`MicroServicePbx::handle_subscriber_*`) call without
 * depending on `SipBridge` directly — keeping the handlers unit-testable
 * against a recording fake. Production wires it to `SipBridge`, which
 * emits a `SUBSCRIBER_REVOKED` SipFrame down the agent tunnel.
 */
class IRevocationSink {
public:
  virtual ~IRevocationSink() = default;

  /// A subscriber was disabled or removed. Implementations signal the
  /// on-prem agent to drop that subscriber's live calls. Best-effort —
  /// the agent may be disconnected; the cloud-side guards (session-row
  /// deletion + the `/sip-ws` status gate) are the durable defence.
  virtual void revoke(const std::string &society_id,
                      const std::string &sip_username) = 0;
};

#endif // REVOCATION_SINK_HPP

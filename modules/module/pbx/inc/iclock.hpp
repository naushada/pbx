#ifndef PBX_ICLOCK_HPP
#define PBX_ICLOCK_HPP

#include <cstdint>

/// Pluggable wall-clock interface so reactor-driven liveness/expiry
/// logic can be tested with a `ManualClock` against a pinned "now."
///
/// Two cloud-side consumers in the `pbx` module use it today:
///   - `PushSender`           — VAPID JWT `exp` claim
///   - `CloudTunnelEndpoint`  — SipFrame heartbeat tick
///
/// The agent's `CloudConnector::IClock` is intentionally NOT this header
/// (different module, different include path); the two interfaces are
/// identical by shape — duplicating to avoid pulling cloud headers into
/// the agent's tighter include set.
class IClock {
public:
  virtual ~IClock() = default;
  virtual std::int64_t now_unix() const = 0;
};

#endif // PBX_ICLOCK_HPP

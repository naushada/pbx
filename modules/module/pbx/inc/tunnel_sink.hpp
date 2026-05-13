#ifndef TUNNEL_SINK_HPP
#define TUNNEL_SINK_HPP

#include "sip_frame.hpp"
#include <cstdint>
#include <string>

/// Abstract sink for outbound frames on the cloud ↔ pbx-agent mTLS tunnel.
///
/// Same interface on both ends:
///   - On the cloud side, `SipBridge` writes frames into a `TunnelSink` that
///     wraps the agent-bound `ACE_SSL_SOCK_Stream`.
///   - On the on-prem side, `SipFrameDemux` writes frames into a `TunnelSink`
///     that wraps the cloud-bound `ACE_SSL_SOCK_Stream` (the same socket,
///     viewed from the other end).
///
/// Tests on both sides substitute an in-memory `FakeTunnel` that records
/// every frame for assertion.
class TunnelSink {
public:
  virtual ~TunnelSink() = default;

  /// Send one complete frame down the tunnel. Implementations must be
  /// non-blocking; back-pressure handling is the implementation's concern.
  virtual void send_frame(SipFrame::Op op,
                          std::uint32_t stream_id,
                          const std::string &payload) = 0;
};

#endif // TUNNEL_SINK_HPP

#ifndef AGENT_STREAM_HPP
#define AGENT_STREAM_HPP

#include "cloud_tunnel_endpoint.hpp"
#include "ace/Event_Handler.h"
#include "ace/SOCK_Stream.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class InnerTlsServer;
class WsInnerTlsBridge;

/**
 * @file agent_stream.hpp
 * @brief ACE event handler that bridges one cloud-side `/agent` socket to
 *        `CloudTunnelEndpoint`. Symmetric mirror of `BrowserStream`.
 *
 * Lifecycle:
 *   1. `WebConnection::handle_input` accepts the `/agent` WS upgrade and
 *      hands the raw fd here.
 *   2. Constructor wraps the fd in a private `TransportAdapter` and gives
 *      it to `endpoint.on_agent_connected()` — that's how the endpoint
 *      can call `send()` to push frames towards the agent without
 *      knowing anything about ACE.
 *   3. After construction, the caller registers with the reactor via
 *      `register_with_reactor()`.
 *   4. Reactor delivers bytes via `handle_input`; WS frames are decoded
 *      and the payload bytes are forwarded into
 *      `endpoint.on_bytes_received()`.
 *   5. EOF or `handle_close` notifies the endpoint and `delete this`.
 *
 * Lifetime / adapter invariant:
 *   The `TransportAdapter` lives inside the endpoint's
 *   `unique_ptr<IAgentTransport>`. AgentStream holds a non-owning back
 *   pointer `m_adapter` that's nulled at every release point so we
 *   never dereference a destroyed adapter:
 *     - `close_socket` (called via `adapter.close()` from the endpoint):
 *       null `m_adapter` first, then close the socket.
 *     - `notify_disconnect_once` (our path to releasing): detach
 *       (`adapter.m_s = nullptr`) and null `m_adapter` BEFORE calling
 *       `endpoint.on_agent_disconnected()` so any re-entrant close
 *       routed via the adapter sees a stale pointer and is a no-op.
 */

class AgentStream : public ACE_Event_Handler {
public:
  /**
   * @param endpoint     Cloud-side accept-end of the tunnel. Must outlive
   *                     the stream.
   * @param fd           Raw fd from `WebConnection`'s `/agent` hand-off.
   *                     Ownership transfers to this object.
   * @param auto_attach  When true (default), the constructor immediately
   *                     publishes a `TransportAdapter` to the endpoint
   *                     via `on_agent_connected` — matches the legacy
   *                     contract and keeps existing tests working.
   *                     Production paths that need an inner-TLS
   *                     handshake before exposing the transport pass
   *                     `false`, run `setup_inner_tls(...)`, and then
   *                     call `attach()`.
   */
  explicit AgentStream(CloudTunnelEndpoint &endpoint, ACE_HANDLE fd,
                        bool auto_attach = true);
  ~AgentStream() override;

  AgentStream(const AgentStream &) = delete;
  AgentStream &operator=(const AgentStream &) = delete;

  /// Drive the cloud-side InnerTLS accept handshake synchronously
  /// against the agent's already-WS-upgraded socket. The blocking read
  /// path lives in `WsInnerTlsBridge` so the same code drives the
  /// handshake here and the steady-state queue post-handshake. Must be
  /// called before `attach()` and `register_with_reactor()`.
  ///
  /// All three paths are required for a useful inner-TLS endpoint —
  /// cert+key for the server's identity, CA for verifying the agent's
  /// client cert. Returns false if cert loading, handshake, or peer
  /// verification fails. Caller should `delete this` and close the fd
  /// on false.
  bool setup_inner_tls(const std::string &cert_path,
                        const std::string &key_path,
                        const std::string &ca_path);

  /// Publish the `TransportAdapter` to the endpoint. Idempotent.
  /// Production (with inner TLS): called only after `setup_inner_tls()`
  /// returns true. Default-constructed (no inner TLS): the constructor
  /// already attached when `auto_attach=true`.
  void attach();

  /// Register with the calling reactor for `READ_MASK`. Production calls
  /// this immediately after `attach()`; tests skip it and drive
  /// `handle_input` directly.
  int register_with_reactor();

  // ── ACE_Event_Handler ──────────────────────────────────────────────────

  int handle_input(ACE_HANDLE) override;
  int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  /// Reactor timer callback. Sends a WS PING so Heroku's router doesn't
  /// H15-drop an otherwise-idle /agent tunnel. See register_with_reactor().
  int handle_timeout(const ACE_Time_Value &tv, const void *act) override;
  ACE_HANDLE get_handle() const override { return m_handle; }

  // ── Adapter callbacks (private impl details exposed for tests) ─────────

  /// Encode @p sip_ws_bytes as a WS text frame and write to the socket.
  /// Returns false if the socket is gone or the write failed.
  bool write_bytes(const std::string &sip_ws_bytes);

  /// Close the socket. Idempotent. Called via the adapter from
  /// `endpoint.on_agent_disconnected()` (or directly by the destructor).
  void close_socket();

private:
  /// `IAgentTransport` impl owned by the endpoint. Holds a back pointer
  /// to the AgentStream; the AgentStream nulls it before releasing the
  /// adapter so a re-entrant call is safe.
  struct TransportAdapter : public IAgentTransport {
    AgentStream *m_s;
    explicit TransportAdapter(AgentStream *s) : m_s(s) {}
    bool send(const std::string &bytes) override {
      return m_s ? m_s->write_bytes(bytes) : false;
    }
    void close() override {
      if (m_s) m_s->close_socket();
    }
  };

  bool drain_frames();
  void notify_disconnect_once();
  /// Cancel the keep-alive ping timer if one is scheduled. Idempotent —
  /// safe to call from handle_close, close_socket, and the destructor.
  void cancel_ping_timer();

  CloudTunnelEndpoint     &m_endpoint;
  ACE_HANDLE               m_handle;
  ACE_SOCK_Stream          m_stream;
  TransportAdapter        *m_adapter; // non-owning; endpoint owns via unique_ptr
  std::vector<std::uint8_t> m_recv_buf;
  bool                     m_close_notified = false;
  bool                     m_attached = false; // set by attach()
  long                     m_ping_timer = -1; // reactor timer id; -1 = unscheduled

  /// Inner-TLS layer (optional). Order matters: the inner-TLS object
  /// holds a reference to *m_bridge, so the bridge must outlive it.
  /// Member declaration order (bridge first) makes destruction order
  /// automatic: m_inner_tls dies before m_bridge.
  std::unique_ptr<WsInnerTlsBridge> m_bridge;
  std::unique_ptr<InnerTlsServer>   m_inner_tls;
};

#endif // AGENT_STREAM_HPP

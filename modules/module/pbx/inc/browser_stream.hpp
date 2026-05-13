#ifndef BROWSER_STREAM_HPP
#define BROWSER_STREAM_HPP

#include "sip_bridge.hpp"
#include "ace/Event_Handler.h"
#include "ace/SOCK_Stream.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file browser_stream.hpp
 * @brief ACE event handler that bridges one browser's SIP-over-WebSocket
 *        socket to the cloud's `SipBridge`.
 *
 * Construction:
 *   1. `WebConnection` accepts the `/sip-ws` WS upgrade.
 *   2. The raw fd is handed to a freshly-constructed `BrowserStream`.
 *   3. The stream calls `bridge.on_browser_upgrade(this, open_meta)` to
 *      register itself; the returned stream-id is remembered.
 *   4. The stream registers itself with the reactor for read events via
 *      `register_with_reactor()`.
 *
 * Lifetime:
 *   The stream is heap-allocated; ACE owns its lifetime once registered.
 *   `handle_close` calls `bridge.on_browser_close(...)` and then
 *   `delete this`. The bridge's `BrowserSink*` is dereferenced only
 *   between `on_browser_upgrade` and `on_browser_close`, so the pointer
 *   is always valid.
 *
 * Threading:
 *   Reactor-thread single-threaded. `BrowserStream` is touched from the
 *   reactor thread only.
 */

class BrowserStream : public ACE_Event_Handler, public BrowserSink {
public:
  /**
   * @param bridge     Cloud-side multiplexer. Must outlive the stream.
   * @param fd         Raw fd from `WebConnection`'s `/sip-ws` hand-off.
   *                   Ownership transfers to this object.
   * @param open_meta  JSON metadata passed to `bridge.on_browser_upgrade`
   *                   (e.g. `{"sipUsername":"u_alice"}`).
   */
  BrowserStream(SipBridge &bridge, ACE_HANDLE fd,
                 const std::string &open_meta);

  ~BrowserStream() override;

  BrowserStream(const BrowserStream &) = delete;
  BrowserStream &operator=(const BrowserStream &) = delete;

  /// Register with the calling reactor for `READ_MASK`. Production calls
  /// this immediately after construction; tests skip it and drive
  /// `handle_input` directly.
  int register_with_reactor();

  /// Stream-id assigned by `SipBridge::on_browser_upgrade()`.
  std::uint32_t stream_id() const { return m_stream_id; }

  // ── ACE_Event_Handler ──────────────────────────────────────────────────

  int handle_input(ACE_HANDLE) override;
  int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  ACE_HANDLE get_handle() const override { return m_handle; }

  // ── BrowserSink ────────────────────────────────────────────────────────

  void send_bytes(const std::string &sip_ws_bytes) override;
  void close(const std::string &reason) override;

private:
  /// Pull as many complete WS frames out of `m_recv_buf` as possible,
  /// dispatching each to the bridge (binary → on_browser_data, close →
  /// on_browser_close, ping → pong reply). Returns false on a peer-closed
  /// or protocol-violation condition (caller should release the stream).
  bool drain_frames();

  /// Single point that tears down the bridge mapping. Idempotent.
  void notify_close_once(const std::string &reason);

  SipBridge       &m_bridge;
  std::uint32_t    m_stream_id = 0;
  ACE_HANDLE       m_handle;
  ACE_SOCK_Stream  m_stream;
  std::vector<std::uint8_t> m_recv_buf;
  bool             m_close_notified = false;
};

#endif // BROWSER_STREAM_HPP

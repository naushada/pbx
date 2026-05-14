#ifndef CLOUD_CONNECTOR_HPP
#define CLOUD_CONNECTOR_HPP

#include "sip_frame.hpp"
#include "tunnel_sink.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

class SipFrameDemux;

/**
 * @file cloud_connector.hpp
 * @brief Agent-side dialer for the cloud ↔ pbx-agent mTLS tunnel.
 *
 * Owns the persistent outbound connection from pbx-agent to Heroku's
 * `/agent` endpoint. Implements `TunnelSink` so the local `SipFrameDemux`
 * (and the eventual `AriClient` push/CDR producer) can write frames
 * upstream without knowing about ACE or TLS.
 *
 * Pure logic — connect attempts, reconnect backoff, and outbound
 * frame buffering. The actual ACE_SSL_SOCK_Connector handshake lives in
 * a concrete `ITransport` implementation injected at construction time;
 * tests substitute an in-memory `FakeTransport`.
 *
 * The class is reactor-thread single-threaded. Production wraps it in
 * an ACE timer that calls `tick()` on the reactor thread; tests step a
 * `ManualClock` and call `tick()` directly.
 *
 * See `DESIGN.md §7` for the tunnel framing protocol and `TDD-PLAN.md`
 * Layer 2 for behavioural contracts.
 */

/// Single open connection to the cloud. Production: an `ACE_SSL_SOCK_Stream`
/// wrapper. Tests: an in-memory recorder that surfaces injected disconnects.
class ITransport {
public:
  virtual ~ITransport() = default;

  /// Write raw bytes upstream. Returns false if the transport is broken
  /// (caller should close + treat as disconnect).
  virtual bool send(const std::string &bytes) = 0;

  /// Close the underlying socket. Idempotent.
  virtual void close() = 0;
};

/// Factory abstraction for a synchronous-from-the-state-machine's-view
/// connect attempt. Production: wraps `ACE_SSL_SOCK_Connector`. Tests:
/// returns a `FakeTransport` configured to succeed or fail.
class ITransportFactory {
public:
  virtual ~ITransportFactory() = default;

  /// Attempt a fresh outbound connection. Returns the connected transport
  /// on success, `nullptr` on failure (causes a backoff retry).
  virtual std::unique_ptr<ITransport>
  create_connected(const std::string &host, std::uint16_t port,
                   const std::string &client_cert_path,
                   const std::string &client_key_path,
                   const std::string &server_ca_path) = 0;
};

/// Clock abstraction — lets tests step time forward without sleeping.
class IClock {
public:
  virtual ~IClock() = default;
  virtual std::int64_t now_unix() const = 0;
};

class CloudConnector : public TunnelSink {
public:
  struct Config {
    /// Cloud rendezvous address. e.g. host="pabx-5fbf3550f938.herokuapp.com",
    /// port=443.
    std::string host;
    std::uint16_t port = 443;

    /// mTLS material — paths to PEM-encoded client cert / key and the
    /// trusted server CA used to verify the cloud's certificate.
    std::string client_cert_path;
    std::string client_key_path;
    std::string server_ca_path;

    /// Exponential backoff. Starts at `initial_backoff_sec` on first
    /// failure, doubles each retry, capped at `max_backoff_sec`.
    /// Successful connect resets it.
    int initial_backoff_sec = 1;
    int max_backoff_sec     = 30;

    /// Outbound frames are buffered while disconnected so we don't lose
    /// CDR / PUSH_NOTIFY events during a brief tunnel flap. `0` means
    /// unbounded (society loads are small in v1; tighten in v2 if needed).
    std::size_t outbound_buffer_max = 0;

    /// SipFrame-level heartbeat (DESIGN.md §7). After `heartbeat_interval_sec`
    /// of inbound silence the connector sends a connection-level `PING`; if
    /// `heartbeat_max_missed` consecutive PINGs go unanswered (no bytes
    /// received at all in between) the tunnel is declared dead and dropped,
    /// which arms the reconnect path. This catches a peer that has hung
    /// without closing the socket — the WS-level keep-alive (PR #1) keeps the
    /// transport open; this proves the peer behind it is still answering.
    /// `heartbeat_interval_sec <= 0` disables the heartbeat (used by tests).
    int heartbeat_interval_sec = 15;
    int heartbeat_max_missed   = 3;
  };

  CloudConnector(Config cfg, ITransportFactory &factory, IClock &clock);
  ~CloudConnector() override = default;

  CloudConnector(const CloudConnector &) = delete;
  CloudConnector &operator=(const CloudConnector &) = delete;

  /// Optional: bind a SipFrameDemux so `on_bytes_received()` forwards
  /// incoming tunnel bytes into it. If unset, incoming bytes are dropped.
  void attach_demux(SipFrameDemux *demux);

  // ── TunnelSink ────────────────────────────────────────────────────────

  /// Encode + send a frame. If currently disconnected the encoded frame
  /// is buffered and flushed on the next successful connect.
  void send_frame(SipFrame::Op op, std::uint32_t stream_id,
                  const std::string &payload) override;

  // ── State machine driver ──────────────────────────────────────────────

  /// Drive the connect / reconnect state machine. Production: called by
  /// an ACE timer once per second on the reactor thread. Tests: invoked
  /// directly after advancing a `ManualClock`.
  ///
  /// Effects:
  ///   - If disconnected and `now >= next_reconnect_at()`: attempt a
  ///     fresh connect via the factory. On success, flush the buffer.
  ///     On failure, increase the backoff up to `max_backoff_sec`.
  void tick();

  // ── External signals (production: wired from ACE event handlers) ──────

  /// Bytes arrived from the cloud. Forwards into the attached demux.
  void on_bytes_received(const std::string &bytes);

  /// Transport reported a fatal error / EOF. Closes the current transport
  /// and arms the backoff timer.
  void on_transport_lost();

  // ── Observability ─────────────────────────────────────────────────────

  bool          connected()           const;
  int           reconnect_attempts()  const { return m_reconnect_attempts; }
  int           current_backoff_sec() const { return m_current_backoff_sec; }
  std::int64_t  next_reconnect_at()   const { return m_next_reconnect_at; }
  std::size_t   buffered_frame_count() const { return m_outbound.size(); }

  /// Heartbeat PINGs sent since the last inbound bytes. Resets to 0 on any
  /// `on_bytes_received()` and on every (re)connect. Reaches
  /// `Config::heartbeat_max_missed` only when the peer has gone silent.
  int           pings_outstanding()   const { return m_pings_outstanding; }

private:
  void attempt_connect();
  void mark_disconnected();
  void flush_outbound();
  /// Called from `tick()` while connected: sends a heartbeat PING once per
  /// `heartbeat_interval_sec` of inbound silence, and drops the tunnel once
  /// `heartbeat_max_missed` PINGs have gone unanswered.
  void maybe_heartbeat();

  Config             m_cfg;
  ITransportFactory &m_factory;
  IClock            &m_clock;

  std::unique_ptr<ITransport>   m_transport;
  SipFrameDemux                *m_demux = nullptr;

  int           m_reconnect_attempts  = 0;
  int           m_current_backoff_sec = 0;
  std::int64_t  m_next_reconnect_at   = 0; // 0 = "connect on next tick"
  std::deque<std::string> m_outbound;

  std::int64_t  m_last_heartbeat_unix = 0; // when the last heartbeat PING went out
  int           m_pings_outstanding   = 0; // PINGs sent with no inbound bytes since
};

#endif // CLOUD_CONNECTOR_HPP

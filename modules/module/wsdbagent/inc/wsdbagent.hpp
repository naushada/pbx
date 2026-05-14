#ifndef WSDBAGENT_HPP
#define WSDBAGENT_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ace/INET_Addr.h"
#include "ace/SOCK_Connector.h"
#include "ace/SOCK_Stream.h"
#include "ace/SSL/SSL_SOCK_Connector.h"
#include "ace/SSL/SSL_SOCK_Stream.h"

#include "dbproto.hpp"
#include "innertls.hpp"
#include "mongodbc.hpp"
#include "wstransport.hpp"

/**
 * @brief WebSocket DB agent — runs on the MongoDB machine and connects
 *        outbound to the Heroku-hosted uniservice on /ws/db.
 *
 * On connect it performs the RFC 6455 upgrade handshake, then enters a
 * receive loop.  Each binary frame carries a BSON-encoded DbRequest;
 * the agent dispatches it to the local MongodbClient and returns a
 * BSON-encoded DbResponse in a new binary frame.
 *
 * Frames from agent to server are masked (RFC 6455 §5.3 client rule).
 * Frames from server to agent are unmasked.
 *
 * On disconnect the agent waits @p backoff_secs seconds and reconnects.
 * Call stop() to request a clean exit; the current backoff sleep will be
 * interrupted and run() returns.
 */
class WsDbAgent {
public:
  /**
   * @param host        Heroku hostname, e.g. @c "myapp.herokuapp.com".
   * @param port        TCP port (443 for WSS, 8080 for local plain WS).
   * @param use_ssl     @c true to use TLS (production); @c false for plain TCP.
   * @param db_uri      MongoDB connection URI.
   * @param db_pool     Connection pool size (as string, passed to MongodbClient).
   * @param db_name     Database name.
   */
  WsDbAgent(std::string host, std::uint16_t port, bool use_ssl,
            std::string db_uri, std::string db_pool, std::string db_name,
            std::string tls_ca = {}, std::string tls_cert = {}, std::string tls_key = {},
            std::string tls_hostname = {});
  ~WsDbAgent();

  /**
   * @brief Connect, run, and automatically reconnect until stop() is called.
   * @param backoff_secs Seconds to wait between reconnection attempts.
   */
  void run(int backoff_secs = 5);

  /// Signal run() to exit after the current session ends (or backoff wait).
  void stop();

private:
  bool connect_and_handshake();
  bool setup_inner_tls();
  void run_session();
  void disconnect();

  std::vector<std::uint8_t> dispatch(const dbproto::DbRequest &req);

  // WebSocket frame I/O (masked=true for client→server)
  bool ws_send(const std::vector<std::uint8_t> &payload, uint8_t opcode = 0x02);
  bool ws_recv_frame(std::uint8_t &opcode, std::vector<std::uint8_t> &payload);

  // Low-level socket wrappers (route to SSL or plain stream)
  ssize_t stream_recv_n(void *buf, std::size_t n);
  ssize_t stream_send_n(const void *buf, std::size_t n);
  void    stream_close();

  // Decode BSON bytes to JSON string (reverse of WsMongodbProxy::json_to_bson)
  static std::string bson_to_json(const std::vector<std::uint8_t> &bson);

  std::string       m_host;
  std::uint16_t     m_port;
  bool              m_ssl;
  std::atomic<bool> m_stop {false};

  std::string       m_db_name;
  std::string       m_tls_ca;
  std::string       m_tls_cert;
  std::string       m_tls_key;
  std::string       m_tls_hostname;

  ACE_SOCK_Stream     m_plain_stream;
  ACE_SSL_SOCK_Stream m_ssl_stream;

  std::unique_ptr<WebSocketTransport> m_transport;
  std::unique_ptr<InnerTlsClient>     m_innerTls;

  std::unique_ptr<MongodbClient> m_db;
};

#endif // WSDBAGENT_HPP

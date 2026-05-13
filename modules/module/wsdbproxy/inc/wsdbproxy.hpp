#ifndef WSDBPROXY_HPP
#define WSDBPROXY_HPP

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ace/SOCK_Stream.h"
#include "ace/SSL/SSL_SOCK_Acceptor.h"
#include "ace/SSL/SSL_SOCK_Stream.h"
#include "ace/Task.h"
#include "ace/Task_T.h"

#include "dbproto.hpp"
#include "innertls.hpp"
#include "mongodbc.hpp"
#include "wstransport.hpp"

/**
 * @brief Transport interface used by WsMongodbProxy.
 *
 * Extracted so that WsDbServer can be substituted with a test double in
 * unit tests without needing real sockets.
 */
class IWsDispatcher {
public:
  virtual ~IWsDispatcher() = default;
  /// Send @p bson as a WebSocket binary frame and block until the matching
  /// BSON response frame arrives (or the connection is lost / times out).
  /// Returns an error-response BSON on failure.
  virtual std::vector<std::uint8_t>
  dispatch(const std::vector<std::uint8_t>& bson) = 0;
};

/**
 * @brief ACE_Task that manages the persistent WebSocket connection from the
 *        ws-db-agent.
 *
 * - Listens for exactly ONE WebSocket upgrade request on the `/ws/db` HTTP
 *   endpoint.  A second agent while one is live receives `HTTP/1.1 409
 *   Conflict` and is rejected.
 * - After the upgrade, `WebConnection` calls `on_agent_connected()` to hand
 *   the socket over.  `svc()` then reads WebSocket response frames in a loop
 *   and wakes the corresponding `dispatch()` caller by reqid.
 * - `dispatch()` is called from MicroService worker threads.  It assigns a
 *   unique reqid, writes the request frame (with socket-write mutex), then
 *   blocks on a per-request condition variable for up to 60 s.
 * - On disconnect all in-flight `dispatch()` calls are failed immediately.
 */
class WsDbServer : public ACE_Task<ACE_MT_SYNCH>, public IWsDispatcher {
public:
  /// TLS configuration for the dedicated mTLS agent port.
  struct TlsConfig {
    std::uint16_t port {0};   ///< Port WsDbServer listens on for agent connections.
    std::string   cert;       ///< Server certificate path (PEM).
    std::string   key;        ///< Server private key path (PEM).
    std::string   ca;         ///< CA certificate path used to verify client certs (PEM).
  };

  /// @param dispatch_timeout_s Seconds before dispatch() gives up (default 60).
  explicit WsDbServer(int dispatch_timeout_s = 60);

  /// Heroku mode with inner TLS (cert/key required for TLS-over-WebSocket).
  /// @param inner_cert  PEM server certificate path for inner TLS.
  /// @param inner_key   PEM server private key path for inner TLS.
  /// @param inner_ca    PEM CA certificate path for client cert verification
  ///                    (empty = no client cert required).
  /// @param dispatch_timeout_s Seconds before dispatch() gives up (default 60).
  explicit WsDbServer(std::string inner_cert, std::string inner_key,
                      std::string inner_ca = {},
                      int dispatch_timeout_s = 60);

  /// mTLS constructor: WsDbServer self-accepts on tls.port with mutual TLS.
  /// Inner TLS reuses the same cert/key/ca from TlsConfig.
  explicit WsDbServer(TlsConfig tls, int dispatch_timeout_s = 60);
  virtual ~WsDbServer();

  int open(void* args = nullptr) override;
  int svc() override;
  int close(u_long flags = 0) override;

  /**
   * @brief Hand the accepted WebSocket socket to the server.
   *
   * Called by WebConnection after a successful `/ws/db` upgrade handshake.
   * If an agent is already connected the socket is rejected with 409 and
   * closed before this method returns.
   *
   * @param handle  Raw socket descriptor (ACE_HANDLE) of the agent connection.
   * @return true if accepted; false if rejected (409 sent, socket closed).
   */
  bool on_agent_connected(ACE_HANDLE handle);

  /// @return true while a ws-db-agent is connected.
  bool is_connected() const { return m_connected.load(); }

  std::vector<std::uint8_t>
  dispatch(const std::vector<std::uint8_t>& bson) override;

private:
  void run_session();
  bool setup_inner_tls();
  void fail_all_pending(const std::string& reason);

  bool ws_upgrade_server();  // read WS upgrade request and send 101 (TLS mode)
  bool ws_send(const std::vector<std::uint8_t>& frame);
  bool ws_recv_frame(std::uint8_t& opcode, std::vector<std::uint8_t>& payload);

  struct PendingRequest {
    std::vector<std::uint8_t> response;
    bool                      ready  {false};
    bool                      failed {false};
    std::mutex                mu;
    std::condition_variable   cv;
  };

  int                   m_timeoutSecs;
  ACE_HANDLE            m_agentHandle {ACE_INVALID_HANDLE};
  ACE_SOCK_Stream       m_agentStream;

  bool                  m_tls_mode {false};
  TlsConfig             m_tls;
  ACE_SSL_SOCK_Acceptor m_ssl_acceptor;
  ACE_SSL_SOCK_Stream   m_ssl_agentStream;

  std::atomic<bool>     m_connected {false};
  std::atomic<bool>     m_running   {false};

  std::mutex              m_connectMu;
  std::condition_variable m_connectCv;

  // Inner TLS
  std::string               m_innerCert;
  std::string               m_innerKey;
  std::string               m_innerCa;
  std::unique_ptr<WebSocketTransport> m_transport;
  std::unique_ptr<InnerTlsServer>      m_innerTls;
  std::atomic<bool>                    m_innerTlsReady {false};

  std::mutex              m_writeMu;   // guards ws_send()
  std::mutex              m_pendingMu; // guards m_pending
  std::atomic<std::int32_t> m_nextReqId {0};
  std::map<std::int32_t, std::shared_ptr<PendingRequest>> m_pending;
};

/**
 * @brief IMongodbClient implementation that forwards every call through a
 *        WsDbServer (or any IWsDispatcher) as a BSON-over-WebSocket request.
 *
 * Instantiated by webservice_main.cpp when `--remote-db` is set.  All method
 * signatures are identical to MongodbClient so MicroService handlers need no
 * changes.
 */
class WsMongodbProxy : public IMongodbClient {
public:
  explicit WsMongodbProxy(IWsDispatcher& dispatcher);

  const std::string& get_database() const override {
    static const std::string empty;
    return empty;
  }

  std::string create_document(const std::string& dbName,
                              const std::string& coll,
                              const std::string& doc) override;

  std::int32_t create_bulk_document(const std::string& dbName,
                                    const std::string& coll,
                                    const std::string& doc) override;

  bool update_collection(const std::string& coll,
                         const std::string& filter,
                         const std::string& document) override;

  std::int32_t update_bulk_document(const std::string& coll,
                                    const std::vector<std::string>& filter,
                                    const std::vector<std::string>& value) override;

  bool delete_document(const std::string& coll,
                       const std::string& doc) override;

  std::string get_document(const std::string& coll,
                           const std::string& query,
                           const std::string& projection) override;

  std::string get_documents(const std::string& coll,
                            const std::string& query,
                            const std::string& projection) override;

  std::string get_documents(const std::string& coll,
                            const std::string& projection) override;

  std::string next_awbno(const std::string& prefix = "AWB") override;

  std::string store_file(const std::string& filename,
                         const std::string& content_type,
                         const std::vector<std::uint8_t>& data) override;

  std::vector<std::uint8_t> fetch_file(const std::string& filename) override;
  std::vector<std::uint8_t> fetch_file_by_id(const std::string& oid) override;
  bool                      delete_file(const std::string& oid) override;

private:
  /// Send a request and parse the response.  Returns a default-constructed
  /// DbResponse (ok=false) on timeout or disconnect.
  dbproto::DbResponse roundtrip(DbOp op,
                                const std::string& db   = {},
                                const std::string& coll = {},
                                const std::vector<std::uint8_t>& doc  = {},
                                const std::vector<std::uint8_t>& doc2 = {},
                                const std::string& sval = {});

  /// Convert a JSON string to raw BSON bytes for embedding in the envelope.
  static std::vector<std::uint8_t> json_to_bson(const std::string& json);

  IWsDispatcher& m_dispatcher;
};

#endif // WSDBPROXY_HPP

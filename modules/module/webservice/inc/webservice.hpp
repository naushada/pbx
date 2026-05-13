#ifndef WEBSERVICE_HPP
#define WEBSERVICE_HPP

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "ace/Basic_Types.h"
#include "ace/Event_Handler.h"
#include "ace/Get_Opt.h"
#include "ace/INET_Addr.h"
#include "ace/OS_Memory.h"
#include "ace/Reactor.h"
#include "ace/SOCK_Acceptor.h"
#include "ace/SOCK_Stream.h"
#include "ace/SSL/SSL_SOCK.h"
#include "ace/SSL/SSL_SOCK_Connector.h"
#include "ace/SSL/SSL_SOCK_Stream.h"
#include "ace/Semaphore.h"
#include "ace/Signal.h"
#include "ace/Task.h"
#include "ace/Task_T.h"
#include "ace/Thread_Manager.h"
#include "ace/Timer_Queue_T.h"

#include "mongodbc.hpp"
#include "wsdbproxy.hpp"

/* Forward declarations */
class WebServer;
class WsDbServer;

/// HTTP method category used for routing decisions.
enum class MicroServiceType : ACE_UINT16 {
  CREATE,
  UPDATE,
  READ,
  DELETE,
  UNKNOWN
};

/// Named buffer sizes for readable capacity constants.
enum class MemorySize : std::uint32_t {
  SIZE_1KB = 1024,
  SIZE_5KB = (5 * SIZE_1KB),
  SIZE_10KB = (10 * SIZE_1KB),
  SIZE_1MB = (SIZE_1KB * SIZE_1KB),
  SIZE_10MB = (10 * SIZE_1MB),
  SIZE_50MB = (50 * SIZE_1MB)
};

/// Index names for the command-line argument array.
enum class CommandArgumentName : std::uint32_t {
  SERVER_IP = 0,
  SERVER_PORT,
  SERVER_WORKER_NODE,
  DB_URI,
  DB_CONN_POOL,
  DB_NAME,
  EMAIL_FROM_NAME,
  EMAIL_FROM_ID,
  EMAIL_FROM_PASSWORD,
  REMOTE_DB,
  AGENT_PORT,
  TLS_CERT,
  TLS_KEY,
  TLS_CA,
  MIGRATE_PASSWORDS,
  MAX_CMD_ARG
};

/**
 * @brief Worker thread that dequeues and processes HTTP requests.
 *
 * Each @c MicroService instance runs in its own thread (via
 * @c ACE_Task<ACE_MT_SYNCH>) and owns a MongoDB client connection.
 * The owning @c WebServer dispatches incoming requests by round-robin
 * across the pool of @c MicroService workers.
 *
 * @par Request lifecycle
 * @code
 *   WebServer (accept) → enqueue(ACE_Message_Block) → MicroService::svc()
 *                         → process_request() → handle_GET/POST/PUT/DELETE
 *                         → send response bytes on the socket
 * @endcode
 */
class MicroService : public ACE_Task<ACE_MT_SYNCH> {
public:
  int svc(void) override;
  int open(void *args = 0) override;
  int close(u_long flags = 0) override;

  ACE_INT32 handle_signal(int signum, siginfo_t *s, ucontext_t *u) override;

  /**
   * @brief Construct a worker thread.
   * @param thrMgr ACE thread manager that owns this task.
   * @param parent Back-pointer to the owning @c WebServer.
   */
  MicroService(ACE_Thread_Manager *thrMgr, WebServer &parent);

  /// Default constructor — for unit tests only; does not start a thread.
  MicroService();

  virtual ~MicroService();

  /// Return the ACE thread ID assigned to this worker.
  ACE_thread_t selfThreadId() { return (m_threadId); }

  /// Return @c true while the worker thread is in the running state.
  bool isTaskRunning() const {
    std::uint32_t st = ACE_Thread_Manager::ACE_THR_RUNNING;
    ACE_Thread_Manager::instance()->thr_state(m_threadId, st);
    return (ACE_Thread_Manager::ACE_THR_RUNNING == st);
  }

  /// Return a reference to the owning @c WebServer.
  WebServer &webServer() const { return (*m_parent); }

  /**
   * @brief Process a raw HTTP request string and send the response.
   * @param handle  Socket descriptor for the client connection.
   * @param req     Raw HTTP request string.
   * @param dbInst  MongoDB client to use for database operations.
   * @return 0 on success, -1 on error.
   */
  std::int32_t process_request(ACE_HANDLE handle, std::string &req,
                               IMongodbClient &dbInst);

  /** @name HTTP method dispatchers */
  ///@{
  std::string handle_OPTIONS(std::string &in);
  std::string handle_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_shipment_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_account_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_inventory_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_email_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_document_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_config_GET(std::string &in, IMongodbClient &dbInst);
  std::string handle_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_shipment_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_account_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_account_login_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_inventory_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_email_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_document_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_config_POST(std::string &in, IMongodbClient &dbInst);
  std::string handle_PUT(std::string &in, IMongodbClient &dbInst);
  std::string handle_shipment_PUT(std::string &in, IMongodbClient &dbInst);
  std::string handle_inventory_PUT(std::string &in, IMongodbClient &dbInst);
  std::string handle_account_PUT(std::string &in, IMongodbClient &dbInst);
  std::string handle_DELETE(std::string &in, IMongodbClient &dbInst);
  ///@}

  /** @name HTTP response builders */
  ///@{
  /**
   * @brief Build a @c 200 OK response.
   * @param http_body    Response body (may be empty).
   * @param content_type MIME type for the @c Content-Type header.
   * @return Fully-formed HTTP response string.
   */
  std::string build_responseOK(std::string http_body,
                               std::string content_type = "application/json");
  std::string build_responseOK(std::string http_body,
                               std::string content_type,
                               const std::string &cache_control);

  /// Build a @c 201 Created response with no body.
  std::string build_responseCreated();

  /**
   * @brief Map a file extension to its MIME content-type string.
   * @param _ext Lowercase extension without a leading dot, e.g. @c "js".
   * @return Corresponding MIME type, or @c "text/html" as a fallback.
   */
  std::string get_contentType(std::string _ext);
  std::string get_cache_control(std::string fileName, std::string ext);

  /**
   * @brief Build an HTTP error response.
   * @param httpBody Error body (may be empty JSON or plain text).
   * @param error    Status line suffix, e.g. @c "404 Not Found".
   * @return Fully-formed HTTP response string.
   */
  std::string build_responseERROR(std::string httpBody, std::string error);
  ///@}

private:
  bool m_continue;
  ACE_thread_t m_threadId;
  bool m_iAmDone;
  WebServer *m_parent;
};

/**
 * @brief ACE event handler for a single accepted client connection.
 *
 * Registered with the reactor on accept.  Accumulates incoming bytes in
 * @c m_recvBuf until @c Http::message_length() reports a complete message,
 * then enqueues the request on the next @c MicroService worker.  The connection
 * is closed as soon as the peer disconnects or a socket error occurs.
 */
class WebConnection : public ACE_Event_Handler {
public:
  ACE_INT32 handle_input(ACE_HANDLE handle) override;
  ACE_INT32 handle_signal(int signum, siginfo_t *s = 0,
                          ucontext_t *u = 0) override;
  ACE_INT32 handle_close(ACE_HANDLE = ACE_INVALID_HANDLE,
                         ACE_Reactor_Mask = 0) override;
  ACE_HANDLE get_handle() const override;

  /**
   * @brief Construct a connection handler.
   * @param parent Owning @c WebServer instance.
   */
  WebConnection(WebServer &parent, ACE_SOCK_Stream strm, ACE_INET_Addr addr);
  virtual ~WebConnection();

  /// Return the underlying socket descriptor.
  ACE_HANDLE handle() const { return (m_handle); }

  /// Set the socket descriptor.
  void handle(ACE_HANDLE fd) { m_handle = fd; }

  /// Record the remote address of this connection.
  void connAddr(ACE_INET_Addr addr) { m_connAddr = addr; }

  /// Return a pointer to the owning @c WebServer.
  WebServer &parent() { return (m_parent); }

private:
  ACE_HANDLE m_handle;
  ACE_INET_Addr m_connAddr;
  ACE_SOCK_Stream m_stream;
  WebServer &m_parent;
  std::string m_recvBuf;
  bool m_handedOff {false};  // true after WebSocket upgrade — prevents close in handle_close
};

/**
 * @brief Top-level TCP server that accepts connections and owns worker threads.
 *
 * Listens on a configurable IP/port, accepts client connections via ACE
 * Reactor, and distributes work to a pool of @c MicroService workers using
 * round-robin selection.  A single shared @c MongodbClient connection pool
 * is created at startup and passed by reference to each worker.
 *
 * @par Startup sequence
 * @code
 *   WebServer srv(ip, port, workers, dbUri, dbPool, dbName);
 *   srv.start();   // blocks in the reactor event loop
 * @endcode
 */
class WebServer : public ACE_Event_Handler {
public:
  ACE_INT32 handle_timeout(const ACE_Time_Value &tv,
                           const void *act = 0) override;
  ACE_INT32 handle_input(ACE_HANDLE handle) override;
  ACE_INT32 handle_signal(int signum, siginfo_t *s = 0,
                          ucontext_t *u = 0) override;
  ACE_INT32 handle_close(ACE_HANDLE = ACE_INVALID_HANDLE,
                         ACE_Reactor_Mask = 0) override;
  ACE_HANDLE get_handle() const override;

  /**
   * @brief Construct the server in local-MongoDB mode.
   * @param _ip        Bind address (empty string → all interfaces).
   * @param _port      TCP port to listen on.
   * @param workerPool Number of @c MicroService worker threads.
   * @param dbUri      MongoDB connection URI.
   * @param dbConnPool MongoDB connection pool size (as a string).
   * @param dbName     MongoDB database name.
   */
  WebServer(std::string _ip, ACE_UINT16 _port, ACE_UINT32 workerPool,
            std::string dbUri, std::string dbConnPool, std::string dbName);

  /**
   * @brief Construct the server in remote-DB (WebSocket proxy) mode.
   * @param _ip        Bind address.
   * @param _port      TCP listen port.
   * @param workerPool Worker thread count.
   * @param db         Pre-constructed IMongodbClient (e.g. WsMongodbProxy).
   * @param wsServer   Owning WsDbServer (may be null in local mode).
   */
  WebServer(std::string _ip, ACE_UINT16 _port, ACE_UINT32 workerPool,
            std::unique_ptr<IMongodbClient> db,
            std::unique_ptr<WsDbServer> wsServer = nullptr);
  virtual ~WebServer();

  /// Start the reactor event loop; returns when the server stops.
  bool start();
  /// Request a graceful shutdown.
  bool stop();

  /// Return the live connection pool (socket → handler map).
  std::map<ACE_HANDLE, std::unique_ptr<WebConnection>> &connectionPool() {
    return (m_connectionPool);
  }

  /// Return the worker thread pool.
  std::vector<std::unique_ptr<MicroService>> &workerPool() {
    return m_workerPool;
  }

  /**
   * @brief Return an iterator to the next worker in round-robin order.
   *
   * Wraps around to the beginning of the pool when the end is reached.
   */
  std::vector<std::unique_ptr<MicroService>>::const_iterator currentWorker() {
    if (m_currentWorker == std::end(m_workerPool)) {
      m_currentWorker = std::begin(m_workerPool);
      return m_currentWorker;
    }
    ++m_currentWorker;
    if (m_currentWorker == std::end(m_workerPool)) {
      m_currentWorker = std::begin(m_workerPool);
    }
    return m_currentWorker;
  }

  /// Return a raw pointer to the shared MongoDB client (never null after
  /// start()).
  IMongodbClient *mongodbcInst() { return (mMongodbc.get()); }

  /// Return the WsDbServer (null in local-MongoDB mode).
  WsDbServer *wsDbServer() { return m_wsDbServer.get(); }

  /// Return the semaphore used to gate concurrent database access.
  ACE_Semaphore &semaphore() const { return (*m_semaphore.get()); }

private:
  ACE_SOCK_Stream m_stream;
  ACE_INET_Addr m_listen;
  ACE_SOCK_Acceptor m_server;
  bool m_stopMe;
  std::map<ACE_HANDLE, std::unique_ptr<WebConnection>> m_connectionPool;
  std::vector<std::unique_ptr<MicroService>> m_workerPool;
  std::vector<std::unique_ptr<MicroService>>::iterator m_currentWorker;
  std::unique_ptr<IMongodbClient> mMongodbc;
  std::unique_ptr<WsDbServer>     m_wsDbServer;
  std::unique_ptr<ACE_Semaphore>  m_semaphore;
};

#endif // WEBSERVICE_H

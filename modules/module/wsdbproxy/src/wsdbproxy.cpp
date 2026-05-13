#include "wsdbproxy.hpp"

#include <cstring>
#include <functional>
#include <stdexcept>
#include <sys/socket.h>

#include <openssl/ssl.h>

#include "ace/Log_Msg.h"
#include "ace/OS_NS_unistd.h"
#include "ace/SSL/SSL_Context.h"

#include "json.hpp"
#include "wstransport.hpp"
#include "wsframe.hpp"

#include <bsoncxx/json.hpp>

using nlohmann::json;

// ═══════════════════════════════════════════════════════════════════════════════
// WsDbServer
// ═══════════════════════════════════════════════════════════════════════════════

WsDbServer::WsDbServer(int dispatch_timeout_s)
  : m_timeoutSecs(dispatch_timeout_s) {}

WsDbServer::WsDbServer(std::string inner_cert, std::string inner_key,
                       std::string inner_ca, int dispatch_timeout_s)
  : m_timeoutSecs(dispatch_timeout_s)
  , m_innerCert(std::move(inner_cert))
  , m_innerKey(std::move(inner_key))
  , m_innerCa(std::move(inner_ca))
{}

WsDbServer::WsDbServer(TlsConfig tls, int dispatch_timeout_s)
  : m_timeoutSecs(dispatch_timeout_s)
  , m_tls_mode(true)
  , m_tls(std::move(tls))
  , m_innerCert(m_tls.cert)
  , m_innerKey(m_tls.key)
  , m_innerCa(m_tls.ca)
{}

WsDbServer::~WsDbServer() { close(0); }

int WsDbServer::open(void*)
{
  if (m_tls_mode) {
    ACE_SSL_Context *ctx = ACE_SSL_Context::instance();
    ctx->set_mode(ACE_SSL_Context::SSLv23_server);
    if (ctx->certificate(m_tls.cert.c_str(), SSL_FILETYPE_PEM) != 0 ||
        ctx->private_key(m_tls.key.c_str(), SSL_FILETYPE_PEM) != 0 ||
        ctx->load_trusted_ca(m_tls.ca.c_str()) != 0) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbServer:%t] %M %N:%l failed to load TLS certs\n")));
      return -1;
    }
    SSL_CTX_set_verify(ctx->context(),
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       nullptr);
    ACE_INET_Addr addr(m_tls.port);
    if (m_ssl_acceptor.open(addr, 1) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                          "bind failed on port %u\n"), (unsigned)m_tls.port));
      return -1;
    }
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "mTLS agent acceptor on port %u\n"), (unsigned)m_tls.port));
  }

  m_running.store(true);
  if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("%D [WsDbServer:%t] %M %N:%l activate failed\n")));
    return -1;
  }
  return 0;
}

int WsDbServer::close(u_long)
{
  m_running.store(false);
  m_innerTlsReady.store(false);
  m_innerTls.reset();
  m_transport.reset();

  if (m_tls_mode) {
    m_ssl_acceptor.close();  // unblocks accept() in svc()
    std::lock_guard<std::mutex> lock(m_connectMu);
    ACE_HANDLE h = m_ssl_agentStream.get_handle();
    if (h != ACE_INVALID_HANDLE)
      ::shutdown(static_cast<int>(h), SHUT_RDWR);
  } else {
    m_connectCv.notify_all();
    std::lock_guard<std::mutex> lock(m_connectMu);
    if (m_agentHandle != ACE_INVALID_HANDLE)
      ::shutdown(static_cast<int>(m_agentHandle), SHUT_RDWR);
  }

  fail_all_pending("server shutting down");
  wait();
  return 0;
}

// ── svc ───────────────────────────────────────────────────────────────────────

int WsDbServer::svc()
{
  if (m_tls_mode) {
    while (m_running.load()) {
      if (m_ssl_acceptor.accept(m_ssl_agentStream) == -1) {
        if (!m_running.load()) break;
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l SSL accept failed\n")));
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(m_connectMu);
        if (m_connected.load()) {
          const char *rej =
              "HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\n"
              "Connection: close\r\n\r\n";
          m_ssl_agentStream.send_n(rej, std::strlen(rej));
          m_ssl_agentStream.close();
          ACE_DEBUG((LM_DEBUG,
                     ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                              "rejected second agent (409)\n")));
          continue;
        }
        if (!ws_upgrade_server()) {
          ACE_ERROR((LM_ERROR,
                     ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                              "WebSocket upgrade failed\n")));
          m_ssl_agentStream.close();
          continue;
        }
        m_agentHandle = m_ssl_agentStream.get_handle();
        m_connected.store(true);
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                            "mTLS agent connected\n")));
      }
      if (!setup_inner_tls()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                            "inner TLS setup failed\n")));
        m_connected.store(false);
        m_ssl_agentStream.close();
        m_agentHandle = ACE_INVALID_HANDLE;
        continue;
      }
      run_session();
    }
  } else {
    while (m_running.load()) {
      {
        std::unique_lock<std::mutex> ul(m_connectMu);
        m_connectCv.wait(ul, [this] {
          return m_connected.load() || !m_running.load();
        });
      }
      if (!m_running.load()) break;
      if (!setup_inner_tls()) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                            "inner TLS setup failed\n")));
        m_connected.store(false);
        m_agentStream.close();
        m_agentHandle = ACE_INVALID_HANDLE;
        fail_all_pending("inner TLS setup failed");
        continue;
      }
      run_session();
    }
  }
  return 0;
}

// ── ws_upgrade_server ─────────────────────────────────────────────────────────

bool WsDbServer::ws_upgrade_server()
{
  std::string headers;
  headers.reserve(512);
  char c = 0;
  while (headers.size() < 8192) {
    if (m_ssl_agentStream.recv_n(&c, 1) != 1) return false;
    headers += c;
    if (headers.size() >= 4 &&
        headers.substr(headers.size() - 4) == "\r\n\r\n")
      break;
  }

  auto pos = headers.find("Sec-WebSocket-Key:");
  if (pos == std::string::npos) return false;
  pos += 18;
  while (pos < headers.size() && headers[pos] == ' ') ++pos;
  auto end = headers.find("\r\n", pos);
  if (end == std::string::npos) return false;
  std::string key = headers.substr(pos, end - pos);

  std::string rsp =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " + wsframe::accept_key(key) + "\r\n\r\n";

  return m_ssl_agentStream.send_n(rsp.data(), rsp.size())
         == static_cast<ssize_t>(rsp.size());
}

// ── on_agent_connected ────────────────────────────────────────────────────────

bool WsDbServer::on_agent_connected(ACE_HANDLE handle)
{
  std::lock_guard<std::mutex> lock(m_connectMu);

  if (m_connected.load()) {
    // A stale connection is likely still held (e.g. the agent restarted but
    // the proxy didn't forward the FIN).  Force-shut it down so run_session()
    // unblocks and exits, then let the agent retry.
    ::shutdown(static_cast<int>(m_agentHandle), SHUT_RDWR);
    m_connected.store(false);
    m_agentHandle = ACE_INVALID_HANDLE;
    ACE_SOCK_Stream tmp;
    tmp.set_handle(handle);
    const char* rsp = "HTTP/1.1 503 Service Unavailable\r\n"
                      "Retry-After: 2\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n\r\n";
    tmp.send_n(rsp, std::strlen(rsp));
    tmp.close();
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "stale agent evicted, new agent told to retry\n")));
    return false;
  }

  m_agentHandle = handle;
  m_agentStream.set_handle(handle);
  m_connected.store(true);
  m_connectCv.notify_one();
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbServer:%t] %M %N:%l agent connected\n")));
  return true;
}

// ── setup_inner_tls ─────────────────────────────────────────────────────────────

bool WsDbServer::setup_inner_tls()
{
  if (m_innerCert.empty() || m_innerKey.empty()) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "inner TLS cert/key not configured — skipping inner TLS\n")));
    return true;
  }

  auto send_frame = [this](const std::vector<std::uint8_t>& payload,
                            std::uint8_t opcode) -> bool {
    return ws_send(wsframe::encode(payload, opcode, false));
  };
  auto recv_frame = [this](std::uint8_t& opcode,
                            std::vector<std::uint8_t>& payload) -> bool {
    return ws_recv_frame(opcode, payload);
  };

  m_transport = std::make_unique<WebSocketTransport>(
      std::move(send_frame), std::move(recv_frame));

  try {
    m_innerTls = std::make_unique<InnerTlsServer>(
        *m_transport, m_innerCert, m_innerKey, m_innerCa);
  } catch (const std::exception& e) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "failed to create InnerTlsServer: %s\n"), e.what()));
    return false;
  }

  if (!m_innerTls->accept()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "inner TLS accept failed\n")));
    m_innerTls.reset();
    m_transport.reset();
    return false;
  }

  m_innerTlsReady.store(true);
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbServer:%t] %M %N:%l inner TLS established\n")));
  return true;
}

// ── run_session ───────────────────────────────────────────────────────────────

void WsDbServer::run_session()
{
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbServer:%t] %M %N:%l run_session started, "
                      "handle=%d\n"), (int)m_agentHandle));

  while (m_connected.load()) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "waiting for frame from agent\n")));

    std::vector<std::uint8_t> plaintext;

    if (m_innerTls) {
      // Read decrypted plaintext through inner TLS.
      if (!m_innerTls->recv(plaintext)) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                            "inner TLS recv failed — agent disconnected\n")));
        break;
      }
      if (plaintext.empty()) continue;  // ping/pong handled by transport, or SSL_WANT_READ
    } else {
      // No inner TLS — read raw WebSocket frames directly (unmasked).
      std::uint8_t opcode;
      if (!ws_recv_frame(opcode, plaintext)) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                            "ws recv failed — agent disconnected\n")));
        break;
      }
      if (opcode == 0x09) {  // ping → pong
        ws_send(wsframe::encode({}, 0x0A, false));
        continue;
      }
      if (opcode == 0x0A) continue;  // pong (unsolicited)
      if (opcode == 0x08) break;     // close
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "received plaintext %zu bytes\n"),
               plaintext.size()));

    // plaintext is the BSON-encoded DbResponse from the agent.
    dbproto::DbResponse rsp;
    if (!dbproto::parse_response(plaintext, rsp)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                          "malformed response BSON (payload %zu bytes)\n"),
                 plaintext.size()));
      continue;
    }
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                        "response reqid=%d ok=%d\n"),
               rsp.reqid, (int)rsp.ok));
    std::shared_ptr<PendingRequest> pending;
    {
      std::lock_guard<std::mutex> lock(m_pendingMu);
      auto it = m_pending.find(rsp.reqid);
      if (it != m_pending.end()) pending = it->second;
    }
    if (pending) {
      std::lock_guard<std::mutex> lock(pending->mu);
      pending->response = std::move(plaintext);
      pending->ready    = true;
      pending->cv.notify_one();
    } else {
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                          "no pending request for reqid=%d\n"), rsp.reqid));
    }
  }
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbServer:%t] %M %N:%l run_session ended\n")));
  {
    std::lock_guard<std::mutex> lock(m_connectMu);
    m_connected.store(false);
    m_innerTlsReady.store(false);
    m_innerTls.reset();
    m_transport.reset();
    if (m_tls_mode)
      m_ssl_agentStream.close();
    else
      m_agentStream.close();
    m_agentHandle = ACE_INVALID_HANDLE;
  }
  fail_all_pending("agent disconnected");
}

// ── fail_all_pending ──────────────────────────────────────────────────────────

void WsDbServer::fail_all_pending(const std::string& reason)
{
  std::map<std::int32_t, std::shared_ptr<PendingRequest>> snap;
  {
    std::lock_guard<std::mutex> lock(m_pendingMu);
    snap = m_pending;
    m_pending.clear();
  }
  for (auto& [id, p] : snap) {
    std::lock_guard<std::mutex> lock(p->mu);
    p->response = dbproto::make_error_response(id, reason);
    p->failed   = true;
    p->ready    = true;
    p->cv.notify_one();
  }
}

// ── dispatch ──────────────────────────────────────────────────────────────────

std::vector<std::uint8_t>
WsDbServer::dispatch(const std::vector<std::uint8_t>& bson)
{
  if (!m_connected.load()) {
    // Extract reqid from bson for the error response
    dbproto::DbRequest req;
    dbproto::parse_request(bson, req);
    return dbproto::make_error_response(req.reqid, "no agent connected");
  }

  // Parse reqid from the request BSON
  dbproto::DbRequest req;
  if (!dbproto::parse_request(bson, req)) {
    return dbproto::make_error_response(-1, "malformed request");
  }

  // Register as a pending request
  auto pending = std::make_shared<PendingRequest>();
  {
    std::lock_guard<std::mutex> lock(m_pendingMu);
    m_pending[req.reqid] = pending;
  }

  // Send through inner TLS (or raw WebSocket frame if inner TLS not configured).
  bool sent;
  if (m_innerTls) {
    sent = m_innerTlsReady.load() && m_innerTls->send(bson);
  } else {
    sent = ws_send(wsframe::encode(bson, 0x02, false));
  }
  if (!sent) {
    std::lock_guard<std::mutex> lock(m_pendingMu);
    m_pending.erase(req.reqid);
    return dbproto::make_error_response(req.reqid, "send failed");
  }

  // Wait for response
  std::unique_lock<std::mutex> ul(pending->mu);
  bool received = pending->cv.wait_for(
      ul,
      std::chrono::seconds(m_timeoutSecs),
      [&] { return pending->ready; });

  {
    std::lock_guard<std::mutex> lock(m_pendingMu);
    m_pending.erase(req.reqid);
  }

  if (!received) {
    return dbproto::make_error_response(req.reqid, "dispatch timeout");
  }
  return std::move(pending->response);
}

// ── ws_send ───────────────────────────────────────────────────────────────────

bool WsDbServer::ws_send(const std::vector<std::uint8_t>& frame)
{
  std::lock_guard<std::mutex> lock(m_writeMu);
  ssize_t sent = m_tls_mode
      ? m_ssl_agentStream.send_n(frame.data(), frame.size())
      : m_agentStream.send_n(frame.data(), frame.size());
  return sent == static_cast<ssize_t>(frame.size());
}

// ── ws_recv_frame ─────────────────────────────────────────────────────────────

bool WsDbServer::ws_recv_frame(std::uint8_t& opcode,
                                std::vector<std::uint8_t>& payload)
{
  // Read into a local accumulation buffer and use wsframe::decode.
  std::vector<std::uint8_t> buf;

  auto read_n = [&](std::size_t n) -> bool {
    std::size_t start = buf.size();
    buf.resize(start + n);
    ssize_t got = m_tls_mode
        ? m_ssl_agentStream.recv_n(buf.data() + start, n)
        : m_agentStream.recv_n(buf.data() + start, n);
    if (got != static_cast<ssize_t>(n)) {
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [WsDbServer:%t] %M %N:%l "
                          "recv_n wanted %zu got %zd errno=%d\n"),
                 n, got, errno));
      buf.resize(start);
      return false;
    }
    return true;
  };

  // Read minimum header
  if (!read_n(2)) return false;

  bool     masked = (buf[1] & 0x80) != 0;
  uint64_t plen   = buf[1] & 0x7F;

  if (plen == 126) {
    if (!read_n(2)) return false;
  } else if (plen == 127) {
    if (!read_n(8)) return false;
  }

  if (masked && !read_n(4)) return false;

  // Re-compute actual length from fully-read header
  std::vector<std::uint8_t> tmp = buf;
  auto frame_opt = wsframe::decode(tmp); // peek: tmp is consumed
  if (!frame_opt) {
    // Need the payload bytes too
    std::size_t need = 0;
    if (plen == 126) {
      need = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
    } else if (plen == 127) {
      need = 0;
      for (int i = 0; i < 8; ++i) need = (need << 8) | buf[2 + i];
    } else {
      need = plen;
    }
    if (!read_n(need)) return false;
    frame_opt = wsframe::decode(buf);
    if (!frame_opt) return false;
  }

  opcode  = frame_opt->opcode;
  payload = std::move(frame_opt->payload);
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// WsMongodbProxy
// ═══════════════════════════════════════════════════════════════════════════════

WsMongodbProxy::WsMongodbProxy(IWsDispatcher& dispatcher)
  : m_dispatcher(dispatcher) {}

// ── json_to_bson ──────────────────────────────────────────────────────────────

std::vector<std::uint8_t> WsMongodbProxy::json_to_bson(const std::string& json_str)
{
  if (json_str.empty()) return {};
  try {
    auto doc = bsoncxx::from_json(json_str);
    return {doc.view().data(), doc.view().data() + doc.view().length()};
  } catch (...) {
    return {};
  }
}

// ── roundtrip ─────────────────────────────────────────────────────────────────

dbproto::DbResponse WsMongodbProxy::roundtrip(DbOp op,
                                               const std::string& db,
                                               const std::string& coll,
                                               const std::vector<std::uint8_t>& doc,
                                               const std::vector<std::uint8_t>& doc2,
                                               const std::string& sval)
{
  auto [reqid, bson] = dbproto::build_request(op, db, coll, doc, doc2, sval);
  auto rsp_bson = m_dispatcher.dispatch(bson);

  dbproto::DbResponse rsp;
  if (!dbproto::parse_response(rsp_bson, rsp)) {
    rsp.ok     = false;
    rsp.errmsg = "malformed response";
  }
  if (!rsp.ok) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsProxy:%t] %M %N:%l op=%d error: %s\n"),
               static_cast<int>(op), rsp.errmsg.c_str()));
  }
  return rsp;
}

// ── IMongodbClient overrides ──────────────────────────────────────────────────

std::string WsMongodbProxy::create_document(const std::string& /*dbName*/,
                                             const std::string& coll,
                                             const std::string& doc)
{
  auto rsp = roundtrip(DbOp::CREATE_DOCUMENT, {}, coll, json_to_bson(doc));
  return rsp.ok ? rsp.sval : std::string{};
}

std::int32_t WsMongodbProxy::create_bulk_document(const std::string& /*dbName*/,
                                                   const std::string& coll,
                                                   const std::string& doc)
{
  // doc is a JSON object/array; embed raw bytes
  std::vector<std::uint8_t> raw(doc.begin(), doc.end());
  auto rsp = roundtrip(DbOp::CREATE_BULK_DOCUMENT, {}, coll, raw);
  return rsp.ok ? rsp.ival : 0;
}

bool WsMongodbProxy::update_collection(const std::string& coll,
                                        const std::string& filter,
                                        const std::string& document)
{
  auto rsp = roundtrip(DbOp::UPDATE_COLLECTION, {}, coll,
                        json_to_bson(filter), json_to_bson(document));
  return rsp.ok && rsp.bval;
}

std::int32_t WsMongodbProxy::update_bulk_document(
    const std::string& coll,
    const std::vector<std::string>& filter,
    const std::vector<std::string>& value)
{
  // Serialise the string vectors as JSON arrays
  json fa = filter;
  json va = value;
  std::vector<std::uint8_t> doc(fa.dump().begin(),  fa.dump().end());
  std::vector<std::uint8_t> doc2(va.dump().begin(), va.dump().end());
  auto rsp = roundtrip(DbOp::UPDATE_BULK_DOCUMENT, {}, coll, doc, doc2);
  return rsp.ok ? rsp.ival : 0;
}

bool WsMongodbProxy::delete_document(const std::string& coll,
                                      const std::string& doc)
{
  auto rsp = roundtrip(DbOp::DELETE_DOCUMENT, {}, coll, json_to_bson(doc));
  return rsp.ok && rsp.bval;
}

std::string WsMongodbProxy::get_document(const std::string& coll,
                                          const std::string& query,
                                          const std::string& projection)
{
  auto rsp = roundtrip(DbOp::GET_DOCUMENT, {}, coll,
                        json_to_bson(query), json_to_bson(projection));
  return rsp.ok ? rsp.sval : std::string{};
}

std::string WsMongodbProxy::get_documents(const std::string& coll,
                                           const std::string& query,
                                           const std::string& projection)
{
  auto rsp = roundtrip(DbOp::GET_DOCUMENTS_QUERIED, {}, coll,
                        json_to_bson(query), json_to_bson(projection));
  return rsp.ok ? rsp.sval : std::string{};
}

std::string WsMongodbProxy::get_documents(const std::string& coll,
                                           const std::string& projection)
{
  auto rsp = roundtrip(DbOp::GET_DOCUMENTS_ALL, {}, coll,
                        {}, json_to_bson(projection));
  return rsp.ok ? rsp.sval : std::string{};
}

std::string WsMongodbProxy::next_awbno(const std::string& prefix)
{
  auto rsp = roundtrip(DbOp::NEXT_AWBNO, {}, {}, {}, {}, prefix);
  return rsp.ok ? rsp.sval : std::string{};
}

std::string WsMongodbProxy::store_file(const std::string& filename,
                                        const std::string& content_type,
                                        const std::vector<std::uint8_t>& data)
{
  std::string sval = filename + "|" + content_type;
  auto rsp = roundtrip(DbOp::STORE_FILE, {}, {}, data, {}, sval);
  return rsp.ok ? rsp.sval : std::string{};
}

std::vector<std::uint8_t> WsMongodbProxy::fetch_file(const std::string& filename)
{
  auto rsp = roundtrip(DbOp::FETCH_FILE, {}, {}, {}, {}, filename);
  return rsp.ok ? rsp.data : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> WsMongodbProxy::fetch_file_by_id(const std::string& oid)
{
  auto rsp = roundtrip(DbOp::FETCH_FILE_BY_ID, {}, {}, {}, {}, oid);
  return rsp.ok ? rsp.data : std::vector<std::uint8_t>{};
}

bool WsMongodbProxy::delete_file(const std::string& oid)
{
  auto rsp = roundtrip(DbOp::DELETE_FILE, {}, {}, {}, {}, oid);
  return rsp.ok && rsp.bval;
}

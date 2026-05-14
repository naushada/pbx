#include "wsdbagent.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <poll.h>
#include <random>
#include <sstream>
#include <thread>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <bsoncxx/json.hpp>

#include "ace/Log_Msg.h"
#include "ace/SSL/SSL_Context.h"

#include "json.hpp"
#include "wstransport.hpp"
#include "wsframe.hpp"

using nlohmann::json;

// ── helpers ────────────────────────────────────────────────────────────────────

namespace {

std::string base64_encode(const unsigned char *data, std::size_t len)
{
  BIO *bmem  = BIO_new(BIO_s_mem());
  BIO *b64   = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO *chain = BIO_push(b64, bmem);
  BIO_write(chain, data, static_cast<int>(len));
  BIO_flush(chain);
  BUF_MEM *ptr = nullptr;
  BIO_get_mem_ptr(chain, &ptr);
  std::string result(ptr->data, ptr->length);
  BIO_free_all(chain);
  return result;
}

std::string random_ws_key()
{
  unsigned char bytes[16];
  RAND_bytes(bytes, sizeof(bytes));
  return base64_encode(bytes, sizeof(bytes));
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// WsDbAgent
// ═══════════════════════════════════════════════════════════════════════════════

WsDbAgent::WsDbAgent(std::string host, std::uint16_t port, bool use_ssl,
                     std::string db_uri, std::string db_pool, std::string db_name,
                     std::string tls_ca, std::string tls_cert, std::string tls_key,
                     std::string tls_hostname)
  : m_host(std::move(host))
  , m_port(port)
  , m_ssl(use_ssl)
  , m_tls_ca(std::move(tls_ca))
  , m_tls_cert(std::move(tls_cert))
  , m_tls_key(std::move(tls_key))
  , m_tls_hostname(std::move(tls_hostname))
{
  ACE_UNUSED_ARG(db_pool);
  m_db_name = db_name;
  m_db = std::make_unique<MongodbClient>(db_uri);
  if (!db_name.empty()) m_db->set_database(db_name);
}

WsDbAgent::~WsDbAgent() { stop(); }

void WsDbAgent::stop() { m_stop.store(true); }

// ── run ───────────────────────────────────────────────────────────────────────

void WsDbAgent::run(int backoff_secs)
{
  const int base_backoff = backoff_secs > 0 ? backoff_secs : 5;
  const int max_backoff  = 60;
  int       cur_backoff  = base_backoff;

  while (!m_stop.load()) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbAgent] connecting to %s:%u (ssl=%d)\n"),
               m_host.c_str(), m_port, (int)m_ssl));

    bool session_ran = false;

    if (!connect_and_handshake()) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] connection failed, retry in %ds\n"),
                 cur_backoff));
    } else if (!setup_inner_tls()) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] inner TLS setup failed, retry in %ds\n"),
                 cur_backoff));
      disconnect();
    } else if (!m_tls_hostname.empty() && !m_innerTls->verify_hostname(m_tls_hostname)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] server CN mismatch (expected %s), retry in %ds\n"),
                 m_tls_hostname.c_str(), cur_backoff));
      disconnect();
    } else {
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [WsDbAgent] session started\n")));
      run_session();
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [WsDbAgent] session ended\n")));
      session_ran = true;
    }

    if (m_stop.load()) break;

    // Reset backoff after a successful session so a single transient disconnect
    // doesn't drag the next retry into the multi-second range.
    if (session_ran) {
      cur_backoff = base_backoff;
    }
    std::this_thread::sleep_for(std::chrono::seconds(cur_backoff));
    if (!session_ran) {
      cur_backoff = std::min(cur_backoff * 2, max_backoff);
    }
  }
}

// ── connect_and_handshake ─────────────────────────────────────────────────────

bool WsDbAgent::connect_and_handshake()
{
  ACE_INET_Addr addr(m_port, m_host.c_str());

  // Fail fast when Heroku's router accepts the SYN but the dyno is wedged
  // or crashed and never completes the TLS handshake — without this the
  // syscall blocks for ~3 minutes per retry.
  ACE_Time_Value connect_timeout(10);

  // ── TCP connect ─────────────────────────────────────────────────────────────
  if (m_ssl) {
    ACE_SSL_Context *ctx = ACE_SSL_Context::instance();
    ctx->set_mode(ACE_SSL_Context::SSLv23_client);
    if (!m_tls_ca.empty())
      ctx->load_trusted_ca(m_tls_ca.c_str());
    if (!m_tls_cert.empty())
      ctx->certificate(m_tls_cert.c_str(), SSL_FILETYPE_PEM);
    if (!m_tls_key.empty())
      ctx->private_key(m_tls_key.c_str(), SSL_FILETYPE_PEM);
    if (!m_tls_ca.empty())
      SSL_CTX_set_verify(ctx->context(), SSL_VERIFY_PEER, nullptr);

    ACE_SSL_SOCK_Connector conn;
    if (conn.connect(m_ssl_stream, addr, &connect_timeout) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] SSL connect failed (errno=%d)\n"),
                 errno));
      return false;
    }
  } else {
    ACE_SOCK_Connector conn;
    if (conn.connect(m_plain_stream, addr, &connect_timeout) == -1) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] TCP connect failed (errno=%d)\n"),
                 errno));
      return false;
    }
  }

  // ── WebSocket upgrade request ────────────────────────────────────────────────
  std::string key = random_ws_key();
  std::ostringstream req;
  req << "GET /ws/db HTTP/1.1\r\n"
      << "Host: " << m_host << "\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << key << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n"
      << "\r\n";
  std::string req_str = req.str();
  if (stream_send_n(req_str.data(), req_str.size())
        != static_cast<ssize_t>(req_str.size())) {
    ACE_ERROR((LM_ERROR, ACE_TEXT("%D [WsDbAgent] failed to send upgrade\n")));
    stream_close();
    return false;
  }

  // ── Read HTTP response headers until \r\n\r\n ────────────────────────────────
  std::string headers;
  headers.reserve(512);
  char c = 0;
  while (headers.size() < 4096) {
    if (stream_recv_n(&c, 1) != 1) {
      ACE_ERROR((LM_ERROR, ACE_TEXT("%D [WsDbAgent] lost connection during upgrade\n")));
      stream_close();
      return false;
    }
    headers += c;
    if (headers.size() >= 4 &&
        headers.substr(headers.size() - 4) == "\r\n\r\n")
      break;
  }

  // ── Validate 101 ────────────────────────────────────────────────────────────
  if (headers.find("101") == std::string::npos) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbAgent] server did not accept upgrade:\n%s\n"),
               headers.c_str()));
    stream_close();
    return false;
  }

  // ── Validate Sec-WebSocket-Accept ────────────────────────────────────────────
  std::string expected = wsframe::accept_key(key);
  if (headers.find(expected) == std::string::npos) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbAgent] bad Sec-WebSocket-Accept\n")));
    stream_close();
    return false;
  }

  return true;
}

// ── setup_inner_tls ─────────────────────────────────────────────────────────────

bool WsDbAgent::setup_inner_tls()
{
  // Build transport adapter over raw WebSocket frames.
  auto send_frame = [this](const std::vector<std::uint8_t>& payload,
                            std::uint8_t opcode) -> bool {
    return ws_send(payload, opcode);
  };
  auto recv_frame = [this](std::uint8_t& opcode,
                            std::vector<std::uint8_t>& payload) -> bool {
    return ws_recv_frame(opcode, payload);
  };

  m_transport = std::make_unique<WebSocketTransport>(
      std::move(send_frame), std::move(recv_frame));

  m_innerTls = std::make_unique<InnerTlsClient>(*m_transport);
  if (!m_tls_ca.empty())   m_innerTls->set_ca(m_tls_ca);
  if (!m_tls_cert.empty() && !m_innerTls->set_cert(m_tls_cert, m_tls_key)) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbAgent] failed to load client cert %s / %s\n"),
               m_tls_cert.c_str(), m_tls_key.empty() ? "(none)" : m_tls_key.c_str()));
    m_innerTls.reset();
    m_transport.reset();
    return false;
  }

  if (!m_innerTls->handshake()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [WsDbAgent] inner TLS handshake failed\n")));
    m_innerTls.reset();
    m_transport.reset();
    return false;
  }

  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [WsDbAgent] inner TLS established\n")));
  return true;
}

// ── recv_ready ────────────────────────────────────────────────────────────────

// Returns true if the socket has data available within timeout_secs seconds.
// Returns false on timeout.  A closed/errored fd returns true so the caller's
// recv_frame call will immediately see the error.
static bool recv_ready(int fd, int timeout_secs)
{
  struct pollfd pfd = { fd, POLLIN, 0 };
  int r = ::poll(&pfd, 1, timeout_secs * 1000);
  if (r < 0) return true;  // error → let recv detect it
  return r > 0;
}

// ── run_session ───────────────────────────────────────────────────────────────

void WsDbAgent::run_session()
{
  constexpr int PING_INTERVAL_S = 30;

  while (!m_stop.load()) {
    int fd = m_ssl ? static_cast<int>(m_ssl_stream.get_handle())
                   : static_cast<int>(m_plain_stream.get_handle());

    if (!recv_ready(fd, PING_INTERVAL_S)) {
      // No frame for 30 s — send a WebSocket-level ping.
      if (!ws_send({}, 0x09)) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D [WsDbAgent] server not responding, disconnecting\n")));
        break;
      }
      continue;
    }

    // Read decrypted plaintext through inner TLS.
    std::vector<std::uint8_t> plaintext;
    if (!m_innerTls->recv(plaintext)) {
      ACE_DEBUG((LM_DEBUG,
                 ACE_TEXT("%D [WsDbAgent] inner TLS recv failed — disconnecting\n")));
      break;
    }
    if (plaintext.empty()) continue;  // ping/pong handled by transport, or SSL_WANT_READ

    // plaintext is the BSON-encoded DbRequest.
    dbproto::DbRequest req;
    if (!dbproto::parse_request(plaintext, req)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] malformed request BSON\n")));
      continue;
    }
    auto rsp_bson = dispatch(req);
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [WsDbAgent] dispatching reqid=%d rspSize=%zu\n"),
               req.reqid, rsp_bson.size()));
    if (!m_innerTls->send(rsp_bson)) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [WsDbAgent] inner TLS send failed, disconnecting\n")));
      break;
    }
  }
  disconnect();
}

// ── disconnect ────────────────────────────────────────────────────────────────

void WsDbAgent::disconnect()
{
  m_innerTls.reset();
  m_transport.reset();
  stream_close();
}

// ── dispatch ──────────────────────────────────────────────────────────────────

std::vector<std::uint8_t> WsDbAgent::dispatch(const dbproto::DbRequest &req)
{
  dbproto::DbResponse rsp;
  rsp.reqid = req.reqid;
  rsp.ok    = false;

  try {
    switch (req.op) {

    case DbOp::CREATE_DOCUMENT:
      rsp.sval = m_db->create_document(m_db_name, req.coll,
                                        bson_to_json(req.doc));
      rsp.ok   = !rsp.sval.empty();
      break;

    case DbOp::CREATE_BULK_DOCUMENT: {
      // doc carries the raw JSON string bytes
      std::string json_str(req.doc.begin(), req.doc.end());
      rsp.ival = m_db->create_bulk_document(m_db_name, req.coll, json_str);
      rsp.ok   = (rsp.ival >= 0);
      break;
    }

    case DbOp::UPDATE_COLLECTION:
      rsp.bval = m_db->update_collection(req.coll,
                                          bson_to_json(req.doc),
                                          bson_to_json(req.doc2));
      rsp.ok   = true;
      break;

    case DbOp::UPDATE_BULK_DOCUMENT: {
      // doc and doc2 carry JSON-array bytes (from nlohmann::json::dump)
      std::string fa_str(req.doc.begin(),  req.doc.end());
      std::string va_str(req.doc2.begin(), req.doc2.end());
      auto fa = json::parse(fa_str).get<std::vector<std::string>>();
      auto va = json::parse(va_str).get<std::vector<std::string>>();
      rsp.ival = m_db->update_bulk_document(req.coll, fa, va);
      rsp.ok   = (rsp.ival >= 0);
      break;
    }

    case DbOp::DELETE_DOCUMENT:
      rsp.bval = m_db->delete_document(req.coll, bson_to_json(req.doc));
      rsp.ok   = true;
      break;

    case DbOp::GET_DOCUMENT:
      rsp.sval = m_db->get_document(req.coll,
                                     bson_to_json(req.doc),
                                     bson_to_json(req.doc2));
      rsp.ok   = !rsp.sval.empty();
      break;

    case DbOp::GET_DOCUMENTS_QUERIED:
      rsp.sval = m_db->get_documents(req.coll,
                                      bson_to_json(req.doc),
                                      bson_to_json(req.doc2));
      rsp.ok   = true;
      break;

    case DbOp::GET_DOCUMENTS_ALL:
      rsp.sval = m_db->get_documents(req.coll, bson_to_json(req.doc2));
      rsp.ok   = true;
      break;

    case DbOp::NEXT_AWBNO:
      rsp.sval = m_db->next_awbno(req.sval);
      rsp.ok   = !rsp.sval.empty();
      break;

    case DbOp::STORE_FILE: {
      // sval = "filename|content_type"
      auto sep = req.sval.find('|');
      std::string filename     = req.sval.substr(0, sep);
      std::string content_type = (sep != std::string::npos)
                                   ? req.sval.substr(sep + 1) : "";
      rsp.sval = m_db->store_file(filename, content_type, req.doc);
      rsp.ok   = !rsp.sval.empty();
      break;
    }

    case DbOp::FETCH_FILE:
      rsp.data = m_db->fetch_file(req.sval);
      rsp.ok   = !rsp.data.empty();
      break;

    case DbOp::FETCH_FILE_BY_ID:
      rsp.data = m_db->fetch_file_by_id(req.sval);
      rsp.ok   = !rsp.data.empty();
      break;

    case DbOp::DELETE_FILE:
      rsp.bval = m_db->delete_file(req.sval);
      rsp.ok   = true;
      break;

    default:
      rsp.errmsg = "unknown op";
      break;
    }
  } catch (const std::exception &e) {
    rsp.ok     = false;
    rsp.errmsg = e.what();
  }

  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("%D [WsDbAgent] op=%d reqid=%d ok=%d\n"),
             static_cast<int>(req.op), req.reqid, (int)rsp.ok));

  return dbproto::build_response(rsp);
}

// ── ws_send ───────────────────────────────────────────────────────────────────

bool WsDbAgent::ws_send(const std::vector<std::uint8_t> &payload, uint8_t opcode)
{
  auto frame = wsframe::encode(payload, opcode, true);  // masked (client→server)
  return stream_send_n(frame.data(), frame.size())
         == static_cast<ssize_t>(frame.size());
}

// ── ws_recv_frame ─────────────────────────────────────────────────────────────

bool WsDbAgent::ws_recv_frame(std::uint8_t &opcode,
                               std::vector<std::uint8_t> &payload)
{
  std::vector<std::uint8_t> buf;

  auto read_n = [&](std::size_t n) -> bool {
    std::size_t start = buf.size();
    buf.resize(start + n);
    ssize_t got = stream_recv_n(buf.data() + start, n);
    if (got != static_cast<ssize_t>(n)) {
      buf.resize(start);
      return false;
    }
    return true;
  };

  if (!read_n(2)) return false;

  bool     masked = (buf[1] & 0x80) != 0;
  uint64_t plen   = buf[1] & 0x7F;

  if (plen == 126) {
    if (!read_n(2)) return false;
  } else if (plen == 127) {
    if (!read_n(8)) return false;
  }

  if (masked && !read_n(4)) return false;

  std::vector<std::uint8_t> tmp = buf;
  auto frame_opt = wsframe::decode(tmp);

  if (!frame_opt) {
    std::size_t need = 0;
    if (plen == 126) {
      need = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
    } else if (plen == 127) {
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

// ── stream helpers ────────────────────────────────────────────────────────────

ssize_t WsDbAgent::stream_recv_n(void *buf, std::size_t n)
{
  return m_ssl ? m_ssl_stream.recv_n(buf, n)
               : m_plain_stream.recv_n(buf, n);
}

ssize_t WsDbAgent::stream_send_n(const void *buf, std::size_t n)
{
  return m_ssl ? m_ssl_stream.send_n(buf, n)
               : m_plain_stream.send_n(buf, n);
}

void WsDbAgent::stream_close()
{
  if (m_ssl) m_ssl_stream.close();
  else       m_plain_stream.close();
}

// ── bson_to_json ──────────────────────────────────────────────────────────────

std::string WsDbAgent::bson_to_json(const std::vector<std::uint8_t> &bson)
{
  if (bson.empty()) return "{}";
  try {
    bsoncxx::document::view v{bson.data(), bson.size()};
    return bsoncxx::to_json(v);
  } catch (...) {
    return "{}";
  }
}

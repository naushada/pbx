#include "ace_https_client.hpp"

#include "ace/INET_Addr.h"
#include "ace/Log_Msg.h"
#include "ace/SSL/SSL_Context.h"
#include "ace/SSL/SSL_SOCK_Connector.h"
#include "ace/SSL/SSL_SOCK_Stream.h"
#include "ace/Time_Value.h"

#include <openssl/ssl.h>

#include <cstring>
#include <sstream>

namespace {

constexpr std::size_t kReadChunk   = 16384;
constexpr std::size_t kMaxResponse = 4 * 1024 * 1024; // 4 MiB cap

/// One-shot TLS connect to (host, port). Returns true on success;
/// installs the system trust store and sets SSL_VERIFY_PEER.
bool ssl_connect(ACE_SSL_SOCK_Stream &stream, const std::string &host,
                 std::uint16_t port) {
  ACE_SSL_Context *ctx = ACE_SSL_Context::instance();
  ctx->set_mode(ACE_SSL_Context::SSLv23_client);
  // Use OpenSSL's default trust store paths. SSL_CTX_set_default_verify_paths
  // returns 1 on success; on failure we still try the connect (some
  // images don't ship CA bundles in the expected paths, and the test
  // smoke-path doesn't need verification to work).
  SSL_CTX_set_default_verify_paths(ctx->context());
  SSL_CTX_set_verify(ctx->context(), SSL_VERIFY_PEER, nullptr);

  ACE_INET_Addr addr(port, host.c_str());
  ACE_Time_Value timeout(10);
  ACE_SSL_SOCK_Connector conn;
  if (conn.connect(stream, addr, &timeout) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceHttpsClient] connect failed host=%s "
                        "port=%u errno=%d\n"),
               host.c_str(), static_cast<unsigned>(port), errno));
    return false;
  }
  return true;
}

} // namespace

// ── post ─────────────────────────────────────────────────────────────────────

IPushHttpClient::Response
AceHttpsClient::post(const std::string &url,
                      const std::vector<std::pair<std::string, std::string>> &headers,
                      const std::string &body) {
  const ParsedUrl u = parse_url(url);
  if (!u.ok) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceHttpsClient] malformed URL: %s\n"),
               url.c_str()));
    return {0, {}};
  }

  ACE_SSL_SOCK_Stream stream;
  if (!ssl_connect(stream, u.host, u.port)) return {0, {}};

  const std::string req = build_request(u.host, u.path_and_query, headers, body);
  if (stream.send_n(req.data(), req.size())
        != static_cast<ssize_t>(req.size())) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AceHttpsClient] send_n failed\n")));
    stream.close();
    return {0, {}};
  }

  std::string buf;
  buf.reserve(4096);
  char chunk[kReadChunk];
  for (;;) {
    const ssize_t n = stream.recv(chunk, sizeof(chunk));
    if (n <= 0) break;
    if (buf.size() + static_cast<std::size_t>(n) > kMaxResponse) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AceHttpsClient] response exceeded %zu bytes; "
                          "dropping\n"), kMaxResponse));
      stream.close();
      return {0, {}};
    }
    buf.append(chunk, static_cast<std::size_t>(n));
  }
  stream.close();

  return parse_response(buf);
}

// ── parse_url ────────────────────────────────────────────────────────────────

AceHttpsClient::ParsedUrl
AceHttpsClient::parse_url(const std::string &url) {
  ParsedUrl out;
  if (url.compare(0, 8, "https://") != 0) return out;

  const std::string remainder = url.substr(8);
  // host[:port]/path[?query][#fragment]
  const auto path_pos = remainder.find('/');
  const std::string authority =
      (path_pos == std::string::npos) ? remainder
                                       : remainder.substr(0, path_pos);

  if (authority.empty()) return out;

  // Split host:port.
  const auto colon = authority.find(':');
  if (colon == std::string::npos) {
    out.host = authority;
    out.port = 443;
  } else {
    out.host = authority.substr(0, colon);
    try {
      const int p = std::stoi(authority.substr(colon + 1));
      if (p <= 0 || p > 65535) return out;
      out.port = static_cast<std::uint16_t>(p);
    } catch (...) {
      return out;
    }
  }

  if (out.host.empty()) return out;

  out.path_and_query =
      (path_pos == std::string::npos) ? "/" : remainder.substr(path_pos);

  out.scheme = "https";
  out.ok = true;
  return out;
}

// ── build_request ────────────────────────────────────────────────────────────

std::string AceHttpsClient::build_request(
    const std::string &host, const std::string &path,
    const std::vector<std::pair<std::string, std::string>> &headers,
    const std::string &body) {
  std::ostringstream os;
  os << "POST " << path << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n";
  for (const auto &h : headers) {
    // Best-effort sanity: skip Host / Content-Length / Connection if the
    // caller sneaks them in — we control those.
    if (h.first == "Host" || h.first == "Content-Length" ||
        h.first == "Connection")
      continue;
    os << h.first << ": " << h.second << "\r\n";
  }
  os << "Content-Length: " << body.size() << "\r\n"
     << "Connection: close\r\n"
     << "\r\n"
     << body;
  return os.str();
}

// ── parse_response ───────────────────────────────────────────────────────────

IPushHttpClient::Response
AceHttpsClient::parse_response(const std::string &raw) {
  const auto eol = raw.find("\r\n");
  if (eol == std::string::npos) return {0, {}};
  const std::string status_line = raw.substr(0, eol);
  if (status_line.compare(0, 7, "HTTP/1.") != 0) return {0, {}};

  const auto sp1 = status_line.find(' ');
  if (sp1 == std::string::npos) return {0, {}};
  const auto sp2 = status_line.find(' ', sp1 + 1);
  int status = 0;
  try {
    const std::string code =
        (sp2 == std::string::npos) ? status_line.substr(sp1 + 1)
                                    : status_line.substr(sp1 + 1, sp2 - sp1 - 1);
    status = std::stoi(code);
  } catch (...) {
    return {0, {}};
  }

  const auto body_pos = raw.find("\r\n\r\n");
  if (body_pos == std::string::npos) return {status, {}};
  return {status, raw.substr(body_pos + 4)};
}

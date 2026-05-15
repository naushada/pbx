#include "ari_rest_client.hpp"

#include "ace/INET_Addr.h"
#include "ace/Log_Msg.h"
#include "ace/SOCK_Connector.h"
#include "ace/Time_Value.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace {

constexpr std::size_t kReadChunk = 8192;
constexpr std::size_t kMaxResponse = 1 * 1024 * 1024; // 1 MiB cap

inline bool is_unreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

} // namespace

AriRestClient::AriRestClient(Config cfg) : m_cfg(std::move(cfg)) {}

// ── IAriRest ──────────────────────────────────────────────────────────────────

IAriRest::Response
AriRestClient::subscribe(const std::string &app_name,
                          const std::vector<std::string> &event_sources) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_subscribe_request(app_name, event_sources,
                                                    m_cfg.host, basic);
  return do_request(req);
}

IAriRest::Response
AriRestClient::continue_in_dialplan(const std::string &channel_id,
                                     const std::string &context,
                                     const std::string &extension,
                                     int priority) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_continue_request(channel_id, context, extension,
                                                   priority, m_cfg.host, basic);
  return do_request(req);
}

IAriRest::Response
AriRestClient::originate(const std::string &endpoint, const std::string &app,
                          const std::string &app_args,
                          const std::string &channel_id,
                          const std::string &caller_id) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_originate_request(
      endpoint, app, app_args, channel_id, caller_id, m_cfg.host, basic);
  return do_request(req);
}

IAriRest::Response
AriRestClient::create_bridge(const std::string &bridge_id,
                              const std::string &type) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_create_bridge_request(bridge_id, type,
                                                       m_cfg.host, basic);
  return do_request(req);
}

IAriRest::Response
AriRestClient::add_channel_to_bridge(const std::string &bridge_id,
                                      const std::string &channel_id) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_add_channel_request(bridge_id, channel_id,
                                                     m_cfg.host, basic);
  return do_request(req);
}

IAriRest::Response
AriRestClient::hangup(const std::string &channel_id,
                       const std::string &reason) {
  const std::string basic = base64_encode(
      m_cfg.username + ":" + m_cfg.password);
  const std::string req = build_hangup_request(channel_id, reason,
                                                m_cfg.host, basic);
  return do_request(req);
}

// ── do_request ────────────────────────────────────────────────────────────────

IAriRest::Response AriRestClient::do_request(const std::string &raw_request) {
  ACE_SOCK_Stream stream;
  ACE_SOCK_Connector conn;
  ACE_INET_Addr addr(m_cfg.port, m_cfg.host.c_str());
  ACE_Time_Value timeout(5);

  if (conn.connect(stream, addr, &timeout) == -1) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriRestClient] connect failed %s:%u errno=%d\n"),
               m_cfg.host.c_str(), static_cast<unsigned>(m_cfg.port), errno));
    return {0, {}};
  }

  if (stream.send_n(raw_request.data(), raw_request.size())
        != static_cast<ssize_t>(raw_request.size())) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("%D [AriRestClient] send_n failed\n")));
    stream.close();
    return {0, {}};
  }

  // Drain the response. We use Connection: close in the request so the
  // server closes after sending; reading until EOF is sufficient. Cap
  // total bytes to kMaxResponse to prevent runaway memory use on a
  // broken server.
  std::string buf;
  buf.reserve(4096);
  char chunk[kReadChunk];
  for (;;) {
    const ssize_t n = stream.recv(chunk, sizeof(chunk));
    if (n <= 0) break;
    if (buf.size() + static_cast<std::size_t>(n) > kMaxResponse) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("%D [AriRestClient] response exceeded %zu bytes; "
                          "dropping\n"), kMaxResponse));
      stream.close();
      return {0, {}};
    }
    buf.append(chunk, static_cast<std::size_t>(n));
  }
  stream.close();

  return parse_response(buf);
}

// ── build_subscribe_request ──────────────────────────────────────────────────

std::string AriRestClient::build_subscribe_request(
    const std::string &app, const std::vector<std::string> &sources,
    const std::string &host, const std::string &basic_auth) {
  std::ostringstream qs;
  bool first = true;
  for (const auto &s : sources) {
    qs << (first ? "" : "&") << "eventSource=" << url_encode(s);
    first = false;
  }
  std::ostringstream os;
  os << "POST /ari/applications/" << url_encode(app)
     << "/subscription?" << qs.str() << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── build_continue_request ───────────────────────────────────────────────────

std::string AriRestClient::build_continue_request(
    const std::string &channel_id, const std::string &context,
    const std::string &extension, int priority,
    const std::string &host, const std::string &basic_auth) {
  std::ostringstream os;
  os << "POST /ari/channels/" << url_encode(channel_id) << "/continue"
     << "?context="   << url_encode(context)
     << "&extension=" << url_encode(extension)
     << "&priority="  << priority
     << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── build_originate_request ──────────────────────────────────────────────────

std::string AriRestClient::build_originate_request(
    const std::string &endpoint, const std::string &app,
    const std::string &app_args, const std::string &channel_id,
    const std::string &caller_id, const std::string &host,
    const std::string &basic_auth) {
  std::ostringstream os;
  os << "POST /ari/channels"
     << "?endpoint="  << url_encode(endpoint)
     << "&app="       << url_encode(app)
     << "&appArgs="   << url_encode(app_args)
     << "&channelId=" << url_encode(channel_id);
  // An empty callerId param confuses some Asterisk builds — leave it off
  // entirely and let the endpoint's configured CallerID stand.
  if (!caller_id.empty())
    os << "&callerId=" << url_encode(caller_id);
  os << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── build_create_bridge_request ──────────────────────────────────────────────

std::string AriRestClient::build_create_bridge_request(
    const std::string &bridge_id, const std::string &type,
    const std::string &host, const std::string &basic_auth) {
  std::ostringstream os;
  os << "POST /ari/bridges"
     << "?type="     << url_encode(type)
     << "&bridgeId=" << url_encode(bridge_id)
     << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── build_add_channel_request ────────────────────────────────────────────────

std::string AriRestClient::build_add_channel_request(
    const std::string &bridge_id, const std::string &channel_id,
    const std::string &host, const std::string &basic_auth) {
  std::ostringstream os;
  os << "POST /ari/bridges/" << url_encode(bridge_id) << "/addChannel"
     << "?channel=" << url_encode(channel_id)
     << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── build_hangup_request ─────────────────────────────────────────────────────

std::string AriRestClient::build_hangup_request(
    const std::string &channel_id, const std::string &reason,
    const std::string &host, const std::string &basic_auth) {
  std::ostringstream os;
  os << "DELETE /ari/channels/" << url_encode(channel_id)
     << "?reason=" << url_encode(reason)
     << " HTTP/1.1\r\n"
     << "Host: " << host << "\r\n"
     << "Authorization: Basic " << basic_auth << "\r\n"
     << "Content-Length: 0\r\n"
     << "Connection: close\r\n"
     << "\r\n";
  return os.str();
}

// ── parse_response ───────────────────────────────────────────────────────────

IAriRest::Response AriRestClient::parse_response(const std::string &raw) {
  // First line: HTTP/1.x <status> <reason>\r\n
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

  // Body starts after CRLFCRLF.
  const auto body_pos = raw.find("\r\n\r\n");
  if (body_pos == std::string::npos) return {status, {}};
  return {status, raw.substr(body_pos + 4)};
}

// ── url_encode ───────────────────────────────────────────────────────────────

std::string AriRestClient::url_encode(const std::string &s) {
  std::ostringstream os;
  os << std::hex << std::uppercase;
  for (unsigned char c : s) {
    if (is_unreserved(c)) {
      os << static_cast<char>(c);
    } else {
      os << '%';
      if (c < 0x10) os << '0';
      os << static_cast<int>(c);
    }
  }
  return os.str();
}

// ── base64_encode ────────────────────────────────────────────────────────────

std::string AriRestClient::base64_encode(const std::string &in) {
  static const char alpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    const auto b1 = static_cast<unsigned char>(in[i + 1]);
    const auto b2 = static_cast<unsigned char>(in[i + 2]);
    out.push_back(alpha[(b0 >> 2) & 0x3F]);
    out.push_back(alpha[((b0 << 4) | (b1 >> 4)) & 0x3F]);
    out.push_back(alpha[((b1 << 2) | (b2 >> 6)) & 0x3F]);
    out.push_back(alpha[b2 & 0x3F]);
    i += 3;
  }
  if (i < in.size()) {
    const auto b0 = static_cast<unsigned char>(in[i]);
    out.push_back(alpha[(b0 >> 2) & 0x3F]);
    if (i + 1 == in.size()) {
      out.push_back(alpha[(b0 << 4) & 0x3F]);
      out.push_back('=');
      out.push_back('=');
    } else {
      const auto b1 = static_cast<unsigned char>(in[i + 1]);
      out.push_back(alpha[((b0 << 4) | (b1 >> 4)) & 0x3F]);
      out.push_back(alpha[(b1 << 2) & 0x3F]);
      out.push_back('=');
    }
  }
  return out;
}

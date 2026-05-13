#include "sip_parser.hpp"
#include <cstdlib>
#include <sstream>

namespace {

// RFC 3261 §7.3.3 compact form → canonical name.
const std::unordered_map<std::string, std::string> &compact_to_canonical() {
  static const std::unordered_map<std::string, std::string> m = {
      {"l", "content-length"},
      {"v", "via"},
      {"i", "call-id"},
      {"f", "from"},
      {"t", "to"},
      {"m", "contact"},
      {"c", "content-type"},
      {"s", "subject"},
      {"k", "supported"},
      {"e", "content-encoding"},
      {"o", "event"},
  };
  return m;
}

// Headers that can appear multiple times and must retain arrival order
// (RFC 3261 §7.3). Stored lowercased.
bool is_multi_value(const std::string &lc_key) {
  return lc_key == "via" || lc_key == "record-route" || lc_key == "route" ||
         lc_key == "contact" || lc_key == "proxy-authenticate" ||
         lc_key == "www-authenticate";
}

// Resolve compact form to canonical (or pass through if already canonical).
std::string canonicalize(const std::string &lc_key) {
  const auto &table = compact_to_canonical();
  auto it = table.find(lc_key);
  return (it != table.end()) ? it->second : lc_key;
}

} // namespace

Sip::Sip(const std::string &in) {
  m_header = extract_header(in);
  if (m_header.empty()) {
    m_error = true;
    return;
  }

  // Detect chunked — forbidden in SIP. Quick scan over header lines.
  if (m_header.find("Transfer-Encoding") != std::string::npos ||
      m_header.find("transfer-encoding") != std::string::npos) {
    // Need to inspect the value. Parse mime, then check.
  }

  // First line.
  auto crlf = m_header.find("\r\n");
  std::string first_line =
      (crlf != std::string::npos) ? m_header.substr(0, crlf) : m_header;
  parse_first_line(first_line);
  if (m_error)
    return;

  parse_mime_header(m_header);

  // Now that headers are parsed, enforce SIP framing rules.
  if (!get_element("transfer-encoding").empty()) {
    m_error = true;
    return;
  }

  // Body: Content-Length only (canonical or compact `l:` — both resolve).
  const std::string cl = get_element("content-length");
  const std::size_t body_offset = m_header.size();
  if (!cl.empty() && body_offset > 0) {
    try {
      auto n = static_cast<std::size_t>(std::stoul(cl));
      if (body_offset + n <= in.size())
        m_body = in.substr(body_offset, n);
    } catch (...) {
      m_error = true;
    }
  }
}

void Sip::parse_first_line(const std::string &line) {
  // Two shapes:
  //   Request: METHOD<SP>Request-URI<SP>SIP/2.0
  //   Status:  SIP/2.0<SP>code<SP>reason-phrase
  if (line.rfind("SIP/2.0", 0) == 0) {
    // Status line.
    m_isRequest = false;
    auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) {
      m_error = true;
      return;
    }
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
      m_error = true;
      return;
    }
    try {
      m_statusCode = std::stoi(line.substr(sp1 + 1, sp2 - sp1 - 1));
    } catch (...) {
      m_error = true;
      return;
    }
    m_reasonPhrase = line.substr(sp2 + 1);
    return;
  }

  // Request line.
  m_isRequest = true;
  auto first_sp = line.find(' ');
  auto last_sp = line.rfind(' ');
  if (first_sp == std::string::npos || last_sp == first_sp) {
    m_error = true;
    return;
  }
  // Require trailing "SIP/2.0".
  if (line.substr(last_sp + 1) != "SIP/2.0") {
    m_error = true;
    return;
  }
  m_method = line.substr(0, first_sp);
  m_uri = pct_decode(line.substr(first_sp + 1, last_sp - first_sp - 1));
}

void Sip::add_element(std::string key, std::string value) {
  const std::string lc = to_lower(std::move(key));
  const std::string canonical = canonicalize(lc);

  // Always populate the multi-value map in arrival order.
  m_multiMap[canonical].push_back(value);

  // Mirror into the scalar token map under the canonical name. For
  // multi-value headers, only the FIRST instance becomes the scalar
  // representation (the topmost Via for routing).
  if (is_multi_value(canonical)) {
    if (m_tokenMap.find(canonical) == m_tokenMap.end()) {
      m_tokenMap.emplace(canonical, std::move(value));
    }
  } else {
    // For single-value headers, last write wins (matches HTTP semantics).
    m_tokenMap[canonical] = std::move(value);
  }
}

std::string Sip::get_element(const std::string &key) const {
  const std::string canonical = canonicalize(to_lower(key));
  auto it = m_tokenMap.find(canonical);
  return (it != m_tokenMap.end()) ? it->second : std::string{};
}

std::vector<std::string> Sip::get_all(const std::string &key) const {
  const std::string canonical = canonicalize(to_lower(key));
  auto it = m_multiMap.find(canonical);
  return (it != m_multiMap.end()) ? it->second : std::vector<std::string>{};
}

std::size_t Sip::message_length(const std::string &buf) {
  const std::string sep("\r\n\r\n");
  auto sep_pos = buf.find(sep);
  if (sep_pos == std::string::npos)
    return 0;

  const std::size_t header_len = sep_pos + sep.size();

  // Walk header lines for Content-Length / l / Transfer-Encoding.
  std::string cl;
  bool has_te = false;
  std::istringstream iss(buf.substr(0, header_len));
  std::string line;
  std::getline(iss, line); // skip first line
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      break;
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = to_lower(line.substr(0, colon));
    std::string val = (colon + 2 <= line.size()) ? line.substr(colon + 2) : "";
    if (key == "content-length" || key == "l")
      cl = val;
    else if (key == "transfer-encoding")
      has_te = true;
  }

  if (has_te)
    return 0; // chunked refused — caller treats as protocol error

  if (cl.empty())
    return header_len; // no body
  try {
    return header_len + static_cast<std::size_t>(std::stoul(cl));
  } catch (...) {
    return 0;
  }
}

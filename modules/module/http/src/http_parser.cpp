#include "http_parser.hpp"
#include <sstream>
#include <zlib.h>

// Decode an HTTP/1.1 chunked transfer-encoded body.
// Format per RFC 7230 §4.1: hex-size[;ext]\r\n data\r\n ... 0\r\n
// [trailers]\r\n
static std::string decode_chunked(const std::string &in) {
  std::string out;
  std::size_t pos = 0;

  while (pos < in.size()) {
    auto crlf = in.find("\r\n", pos);
    if (crlf == std::string::npos)
      break;

    std::string size_str = in.substr(pos, crlf - pos);
    auto ext = size_str.find(';');
    if (ext != std::string::npos)
      size_str.resize(ext);

    std::size_t chunk_size;
    try {
      chunk_size = std::stoul(size_str, nullptr, 16);
    } catch (...) {
      break;
    }

    pos = crlf + 2;
    if (chunk_size == 0)
      break;
    if (pos + chunk_size > in.size())
      break;

    out.append(in, pos, chunk_size);
    pos += chunk_size + 2;
  }

  return out;
}

// Extract the boundary token from a Content-Type header value.
static std::string multipart_boundary(const std::string &ct) {
  if (ct.find("multipart/form-data") == std::string::npos)
    return {};
  auto pos = ct.find("boundary=");
  if (pos == std::string::npos)
    return {};
  std::string b = ct.substr(pos + 9);
  auto semi = b.find(';');
  if (semi != std::string::npos)
    b.resize(semi);
  while (!b.empty() &&
         (b.back() == ' ' || b.back() == '\r' || b.back() == '\n'))
    b.pop_back();
  return b;
}

// Inflate a gzip or deflate (zlib-wrapped) payload using zlib.
static std::string decompress(const std::string &in,
                              const std::string &encoding) {
  const int windowBits = (encoding == "gzip") ? (15 + 16) : (15 + 32);

  z_stream zs{};
  if (inflateInit2(&zs, windowBits) != Z_OK)
    return {};

  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());

  std::string out;
  int ret;
  do {
    char buf[32768];
    zs.next_out = reinterpret_cast<Bytef *>(buf);
    zs.avail_out = sizeof(buf);
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      ACE_DEBUG((LM_ERROR,
                 ACE_TEXT("%D [worker:%t] %M %N:%l decompress error: %s\n"),
                 zs.msg ? zs.msg : "unknown"));
      inflateEnd(&zs);
      return {};
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
  } while (ret != Z_STREAM_END);

  inflateEnd(&zs);
  return out;
}

Http::Http(const std::string &in) {
  m_header = extract_header(in);
  if (!m_header.empty()) {
    parse_uri(m_header);
    parse_mime_header(m_header);
  }
  m_body = get_body(in);
}

void Http::parse_uri(const std::string &in) {
  auto crlf = in.find("\r\n");
  const std::string req = (crlf != std::string::npos) ? in.substr(0, crlf) : in;

  // Split: METHOD<SP>path[?qs]<SP>HTTP/x.x
  auto first_sp = req.find(' ');
  if (first_sp == std::string::npos)
    return;

  m_method = req.substr(0, first_sp);

  auto last_sp = req.rfind(' ');
  std::string uri_part =
      req.substr(first_sp + 1, (last_sp > first_sp) ? last_sp - first_sp - 1
                                                    : std::string::npos);

  auto q = uri_part.find('?');
  if (q == std::string::npos) {
    m_uriName = pct_decode(uri_part);
    return;
  }

  m_uriName = pct_decode(uri_part.substr(0, q));

  // Parse key=value pairs from the query string
  std::istringstream qs(uri_part.substr(q + 1));
  std::string token;
  while (std::getline(qs, token, '&')) {
    auto eq = token.find('=');
    if (eq == std::string::npos)
      continue;
    add_element(pct_decode(token.substr(0, eq)),
                pct_decode(token.substr(eq + 1)));
  }
}

void Http::dump() const {
  ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l The uriName is %s\n"),
             m_uriName.c_str()));
  for (const auto &kv : m_tokenMap) {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [worker:%t] %M %N:%l param %s value %s\n"),
               kv.first.c_str(), kv.second.c_str()));
  }
}

std::size_t Http::message_length(const std::string &buf) {
  const std::string sep("\r\n\r\n");
  auto sep_pos = buf.find(sep);
  if (sep_pos == std::string::npos)
    return 0;

  const std::size_t header_len = sep_pos + sep.size();

  std::string te, cl, ct;
  std::istringstream iss(buf.substr(0, header_len));
  std::string line;
  std::getline(iss, line); // skip request line
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      break;
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = line.substr(0, colon);
    std::string val = (colon + 2 <= line.size()) ? line.substr(colon + 2) : "";
    if (key == "Transfer-Encoding")
      te = val;
    else if (key == "Content-Length")
      cl = val;
    else if (key == "Content-Type")
      ct = val;
  }

  // Rule 1: chunked transfer encoding — scan for terminal chunk.
  if (te == "chunked") {
    const std::string terminal("0\r\n\r\n");
    auto end_pos = buf.find(terminal, header_len);
    if (end_pos == std::string::npos)
      return 0;
    return end_pos + terminal.size();
  }

  // Rule 2: explicit Content-Length.
  if (!cl.empty()) {
    try {
      return header_len + static_cast<std::size_t>(std::stoul(cl));
    } catch (...) {
      return 0;
    }
  }

  // Rule 3: multipart/form-data without Content-Length.
  {
    std::string boundary = multipart_boundary(ct);
    if (!boundary.empty()) {
      std::string terminal = "--" + boundary + "--";
      auto end_pos = buf.find(terminal, header_len);
      if (end_pos == std::string::npos)
        return 0;
      std::size_t end = end_pos + terminal.size();
      if (end + 2 <= buf.size() && buf[end] == '\r' && buf[end + 1] == '\n')
        end += 2;
      return end;
    }
  }

  // Rule 4: no body framing (GET, DELETE, OPTIONS, …).
  return header_len;
}

std::string Http::get_body(const std::string &in) {
  const std::string te = get_element("Transfer-Encoding");
  const std::string ce = get_element("Content-Encoding");
  const std::string cl = get_element("Content-Length");
  const std::string ct = get_element("Content-Type");

  std::string raw;
  const std::size_t body_offset = m_header.size();

  if (te == "chunked") {
    raw = decode_chunked(in.substr(body_offset));
  } else if (!cl.empty()) {
    if (body_offset > 0)
      raw = in.substr(body_offset, static_cast<std::size_t>(std::stoul(cl)));
  } else {
    std::string boundary = multipart_boundary(ct);
    if (!boundary.empty()) {
      std::string terminal = "--" + boundary + "--";
      auto pos = in.find(terminal, body_offset);
      if (pos != std::string::npos) {
        std::size_t end = pos + terminal.size();
        if (end + 2 <= in.size() && in[end] == '\r' && in[end + 1] == '\n')
          end += 2;
        raw = in.substr(body_offset, end - body_offset);
      }
    }
  }

  if (!raw.empty() && (ce == "gzip" || ce == "deflate"))
    raw = decompress(raw, ce);

  return raw;
}

#ifndef SIP_PARSER_HPP
#define SIP_PARSER_HPP

#include "message_parser.hpp"
#include <vector>

/**
 * @brief Parses a raw SIP/2.0 message (request or status) into its parts.
 *
 * Subclass of `MessageParser`. Adds SIP-specific behaviour on top of the
 * shared MIME-header machinery:
 *   - Discriminates request line (`METHOD Request-URI SIP/2.0`) from
 *     status line (`SIP/2.0 <code> <reason>`).
 *   - Resolves compact header forms (RFC 3261 §7.3.3) — `l`, `v`, `i`, `f`,
 *     `t`, `m`, `c`, `s`, `k`, `e`, `o` — to their canonical names on lookup.
 *   - Preserves arrival order of repeat-allowed headers (`Via`, `Record-Route`,
 *     `Route`, `Contact`, `Proxy-Authenticate`, `WWW-Authenticate`) via a
 *     parallel multi-value map.
 *   - Refuses chunked transfer-encoding (RFC 3261 §7.5 forbids it).
 */
class Sip : public MessageParser {
public:
  Sip() = default;
  explicit Sip(const std::string &in);
  ~Sip() override = default;

  /// True if the first line was `METHOD Request-URI SIP/2.0`.
  bool is_request() const { return m_isRequest; }

  /// True if the input failed to parse (malformed first line, etc.).
  bool error() const { return m_error; }

  // Request-side accessors (valid when is_request() == true)
  const std::string &method() const { return m_method; }
  const std::string &uri() const { return m_uri; }

  // Status-side accessors (valid when is_request() == false)
  int status_code() const { return m_statusCode; }
  const std::string &reason_phrase() const { return m_reasonPhrase; }

  /// Header lookup with compact-form alias fallback.
  /// e.g. `get_element("Content-Length")` resolves a value sent as `l:`.
  /// For multi-value headers, returns the first (topmost) entry.
  std::string get_element(const std::string &key) const override;

  /// Insert preserving order; if the key (or its compact form) is multi-value,
  /// it is also appended to the per-key vector accessed via `get_all()`.
  void add_element(std::string key, std::string value) override;

  /// All values for a (possibly multi-value) header, in arrival order.
  std::vector<std::string> get_all(const std::string &key) const;

  /// Total wire length of the SIP message in @p buf, or 0 if more bytes are
  /// needed. SIP framing rule: CRLFCRLF + Content-Length (canonical or `l:`).
  /// Chunked transfer-encoding is refused (returns 0 and sets m_error if
  /// constructed on such input).
  static std::size_t message_length(const std::string &buf);

private:
  void parse_first_line(const std::string &line);

  bool m_isRequest = false;
  bool m_error = false;

  std::string m_method;
  std::string m_uri;

  int m_statusCode = 0;
  std::string m_reasonPhrase;

  // Per-key arrival-ordered vector for repeat-allowed headers.
  std::unordered_map<std::string, std::vector<std::string>> m_multiMap;
};

#endif // SIP_PARSER_HPP

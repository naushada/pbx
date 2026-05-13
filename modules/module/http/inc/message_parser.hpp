#ifndef MESSAGE_PARSER_HPP
#define MESSAGE_PARSER_HPP

#include <string>
#include <unordered_map>

/// Protocol-agnostic base for HTTP/1.1 and SIP/2.0 message parsing.
///
/// Both protocols share:
///   - "request-line CRLF (header CRLF)* CRLF body" framing,
///   - case-insensitive MIME-style header lookup,
///   - percent-decoded URIs.
///
/// `MessageParser` owns the protocol-agnostic state and helpers. Subclasses
/// (`Http`, `Sip`) implement first-line parsing and protocol-specific body
/// framing.
class MessageParser {
public:
  MessageParser() = default;
  virtual ~MessageParser() = default;

  /// Decoded body bytes (subclasses populate this).
  const std::string &body() const { return m_body; }

  /// Raw header section including the trailing CRLFCRLF.
  const std::string &header() const { return m_header; }

  /// Store a header key/value (or query-string pair). Key is lowercased
  /// before storage; lookups are case-insensitive. Subclasses may override
  /// to add aliasing (e.g. SIP compact headers `l` -> `content-length`).
  virtual void add_element(std::string key, std::string value);

  /// Case-insensitive header (or query-string) lookup. Empty string if
  /// absent. Subclasses may override to consult alias tables.
  virtual std::string get_element(const std::string &key) const;

  /// Find the header section in a raw buffer. Returns the prefix up to and
  /// including CRLFCRLF, or the entire buffer if the separator is not found.
  static std::string extract_header(const std::string &in);

  /// Parse every "Key: Value" line after the first into the token map.
  /// Skips the first line (request or status line).
  void parse_mime_header(const std::string &in);

protected:
  std::unordered_map<std::string, std::string> m_tokenMap;
  std::string m_header;
  std::string m_body;
};

/// Lowercase ASCII helper (file-local utility shared by parser subclasses).
std::string to_lower(std::string s);

/// Decode percent-encoded (%XX) and plus-encoded (+) characters in a URI
/// component. Shared by HTTP query-string parsing and SIP request-URI parsing.
std::string pct_decode(const std::string &s);

#endif // MESSAGE_PARSER_HPP

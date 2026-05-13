#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include "message_parser.hpp"
#include "ace/Log_Msg.h"

/**
 * @brief Parses a raw HTTP/1.1 request message into its constituent parts.
 *
 * Thin subclass of `MessageParser`. Inherits the shared header map,
 * `parse_mime_header`, `get_element`, and `extract_header`. Adds the
 * HTTP-specific first-line parser (METHOD URI HTTP/1.1), query-string
 * splitting, chunked transfer-encoding decode, and gzip/deflate decompression.
 */
class Http : public MessageParser {
public:
  Http() = default;

  /**
   * @brief Parse a complete HTTP/1.1 request message.
   * @param in Raw bytes of the HTTP request, including headers and body.
   */
  explicit Http(const std::string &in);

  ~Http() override = default;

  const std::string &uri() const { return m_uriName; }
  void uri(std::string name) { m_uriName = std::move(name); }
  const std::string &method() const { return m_method; }

  /// Parse the request line into method, URI path, and query params.
  void parse_uri(const std::string &in);

  /// Log parsed fields to the ACE debug log.
  void dump() const;

  /**
   * @brief Determine the total wire length of the HTTP/1.1 message in @p buf.
   * Returns 0 if more bytes are needed.
   */
  static std::size_t message_length(const std::string &buf);

private:
  std::string get_body(const std::string &in);

  std::string m_uriName;
  std::string m_method;
};

#endif // HTTP_PARSER_HPP

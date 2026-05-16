# http — MessageParser base + HTTP/1.1 subclass

The module hosts two related parsers:

| Class            | File                                       | Role                                                                 |
|------------------|--------------------------------------------|----------------------------------------------------------------------|
| `MessageParser`  | `inc/message_parser.hpp` + `src/*.cpp`     | Protocol-agnostic base for any CRLFCRLF-framed, MIME-style message.  |
| `Http`           | `inc/http_parser.hpp` + `src/*.cpp`        | HTTP/1.1 subclass — first-line + chunked + gzip/deflate + multipart. |

`Sip` (in the sibling [`sip/`](../sip/README.md) module) is the other consumer of `MessageParser` and the reason the base exists.

> **Origin.** `Http` was lifted verbatim from the upstream shared-library `modules/module/http/` and then refactored to inherit from a new `MessageParser` base. The 20 inherited GTest cases (`HttpParser*`) are the regression guard — they must remain 100 % green for the refactor to be considered correct. See [`DESIGN.md §12 reuse map`](../../../DESIGN.md#12-reuse-map).

---

## MessageParser (base)

### What it owns

- A lowercased `std::unordered_map<std::string, std::string> m_tokenMap` — every header key is stored lowercase; lookups are case-insensitive.
- The raw header section `m_header` (everything up to and including the trailing `CRLFCRLF`).
- The decoded body `m_body` (subclasses populate this).

### Public API

```cpp
class MessageParser {
public:
  const std::string& header() const;          // raw header bytes
  const std::string& body()   const;          // decoded body

  virtual void add_element(std::string key, std::string value);
  virtual std::string get_element(const std::string& key) const;

  // Static framing helper used by message_length() in subclasses.
  static std::string extract_header(const std::string& in);

  // Parse "Key: Value" lines after the first. Skips the first line
  // (request line or status line); the subclass handles it.
  void parse_mime_header(const std::string& in);
};

// Free helpers, used by Http and Sip.
std::string to_lower(std::string s);
std::string pct_decode(const std::string& s);     // %XX and '+' decode
```

`add_element` and `get_element` are **virtual** so the SIP subclass can layer compact-header aliasing on top without rewriting MIME parsing.

### Behaviour pinned by tests (`test/message_parser_test.cc` — 8 tests)

- `HeaderSeparator_Detected` — `extract_header` returns the prefix that ends in `CRLFCRLF` and excludes the body.
- `HeaderSeparator_ReturnsWholeBufferWhenAbsent` — graceful when input is still incomplete.
- `MimeHeader_LowercasedKeys` — `Host` and `HOST` resolve identically; storage is lowercase.
- `MimeHeader_StopsAtBlankLine` — never consumes body bytes as headers.
- `MimeHeader_SkipsLinesWithoutColon` — malformed lines don't blow up parsing.
- `AddElement_AndLookup` / `GetElement_MissingKeyReturnsEmpty` — case-insensitive map semantics.
- `PctDecode_KnownVectors` — `%20`, `%2F`, and `+` decode per RFC 3986.

---

## Http (subclass)

### Public API

```cpp
class Http : public MessageParser {
public:
  Http() = default;
  explicit Http(const std::string& raw_request);

  const std::string& uri()    const;
  void               uri(std::string name);   // override (used by routing)
  const std::string& method() const;

  void parse_uri(const std::string& in);      // METHOD URI HTTP/1.1
  void dump() const;                          // ACE_DEBUG log

  // Total wire length of the HTTP/1.1 message in @p buf.
  // Returns 0 when more bytes are needed.
  //
  // Rule order:
  //   1. CRLFCRLF not present                          → 0
  //   2. Transfer-Encoding: chunked                    → scan for terminal "0\r\n\r\n"
  //   3. Content-Length: N                             → header_len + N
  //   4. multipart/form-data with closing boundary     → end of boundary marker
  //   5. otherwise (GET, DELETE, OPTIONS, …)           → header_len
  static std::size_t message_length(const std::string& buf);
};
```

Public-API contract: identical to the upstream shared-library `Http`. The inherited tests in `test/httpparser_test.cc` enforce this — any incompatible change re-breaks 20 tests.

### Body handling

- **Transfer-Encoding: chunked** → `decode_chunked()` reassembles the RFC 7230 §4.1 chunked encoding.
- **Content-Encoding: gzip|deflate** → `decompress()` inflates via zlib.
- **multipart/form-data** without `Content-Length` → sliced up to (and including) the closing `--{boundary}--` marker.
- Order matters: Transfer-Encoding is decoded first; Content-Encoding (decompression) is applied to the decoded bytes.

### Behaviour pinned by tests (`test/httpparser_test.cc` — 20 tests, **inherited from the shared library**)

All 20 inherited cases — method/URI extraction across GET/POST/PUT/DELETE/OPTIONS, query-string parsing, percent + plus decoding, MIME headers all parsed, body via Content-Length, chunked decode (single + multi), gzip decompression, gzip-over-chunked, header includes the `CRLFCRLF` separator.

---

## Files

```
http/
  inc/
    message_parser.hpp     # base
    http_parser.hpp        # subclass
  src/
    message_parser.cpp     # base impl
    http_parser.cpp        # subclass impl (chunked, gzip, multipart)
  test/
    message_parser_test.cc # 8 tests
    httpparser_test.cc     # 20 tests (inherited from the shared library, regression guard)
    httpparser_test.hpp    # zlib helper for gzip test vectors
```

## Dependencies

- **ACE** (`ace/Log_Msg.h`) — only for `ACE_DEBUG` in `Http::dump()` and `decompress()` error path. Compile-time include; the base class doesn't depend on ACE.
- **zlib** — gzip/deflate decompression in `Http`. Not used by `MessageParser` or `Sip`.
- **GTest** — tests only.

## Who depends on us

- [`sip/`](../sip/README.md) — `Sip` is a subclass of `MessageParser`. Adds SIP-specific first-line parsing and compact-header aliases.
- `webservice/` (Layer 1) — `WebServer`/`WebConnection`/`MicroService` will call `Http::message_length()` from socket read loops and construct `Http` to parse a complete request.

## Build & test

From repo root:

```sh
podman build -f docker/Dockerfile.test -t onprem-pbx-test:layer0 .
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer0 \
  --gtest_filter='HttpParser.*:MessageParserBase.*'
```

Expected: **28/28 PASSED** (20 HttpParser + 8 MessageParserBase).

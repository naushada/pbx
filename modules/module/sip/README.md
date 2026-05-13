# sip — SIP/2.0 parser

`Sip` is a subclass of [`MessageParser`](../http/README.md). It adds the bits HTTP doesn't need:

- request-vs-status first-line discrimination,
- RFC 3261 §7.3.3 compact-header aliasing,
- multi-value, arrival-ordered storage for headers that may legally repeat,
- refusal of `Transfer-Encoding: chunked` (RFC 3261 §7.5 forbids it).

> **Why a parser at all when `SipBridge` is byte-faithful?** The cloud bridge and the agent demux both treat SIP-WS frames as opaque bytes — they do not parse. `Sip` exists for (1) the Layer 4 test harness, which constructs INVITE/REGISTER bytes and asserts on response lines, and (2) optional cloud-side abuse filtering of `/sip-ws` upgrades. See [`DESIGN.md §11`](../../../DESIGN.md#11-deployment) prose on this.

---

## First-line discrimination

```cpp
Sip s(raw_message);
if (s.error()) { /* malformed */ }

if (s.is_request()) {
    s.method();        // e.g. "INVITE", "REGISTER"
    s.uri();           // e.g. "sip:b-204@society"
} else {
    s.status_code();   // e.g. 200, 486
    s.reason_phrase(); // e.g. "OK", "Busy Here"
}
```

The discrimination is by prefix: a first line starting with `SIP/2.0` is a status; anything else is a request. The classic naïve parser bug (treating a status line as `method() == "SIP/2.0"`) is explicitly pinned by `SipParser.DistinguishesRequestFromStatus`.

---

## Compact-header aliases (RFC 3261 §7.3.3)

The alias table (lowercased throughout):

| Compact | Canonical          |
|---------|--------------------|
| `l`     | `content-length`   |
| `v`     | `via`              |
| `i`     | `call-id`          |
| `f`     | `from`             |
| `t`     | `to`               |
| `m`     | `contact`          |
| `c`     | `content-type`     |
| `s`     | `subject`          |
| `k`     | `supported`        |
| `e`     | `content-encoding` |
| `o`     | `event`            |

Resolution happens **at insert time**. `Sip::add_element` lowercases the key, looks it up in the alias table, and stores the value under the canonical name. Lookups by either form therefore work without per-call alias resolution:

```cpp
Sip s(invite_using_compact_l);
s.get_element("Content-Length"); // ✓
s.get_element("content-length"); // ✓
s.get_element("l");              // ✓ (canonicalized to "content-length")
```

This also means `Sip::message_length()` is **immune** to the most common bite — a UA that sends `l: 142` instead of `Content-Length: 142` — because `message_length()` consults both spellings before deciding how many bytes the message is on the wire.

---

## Multi-value headers

These headers may legally appear more than once and **must retain arrival order** (RFC 3261 §7.3):

`via`, `record-route`, `route`, `contact`, `proxy-authenticate`, `www-authenticate`.

Storage is split:

- `m_tokenMap[canonical]` — the **topmost** (first-arriving) value. Used by `get_element()` so the caller gets the routing-significant entry by default.
- `m_multiMap[canonical]` — `std::vector<std::string>` of every value in arrival order. Used by `get_all()`.

```cpp
Sip s(invite_with_three_via_headers);
s.get_element("Via");       // topmost only
auto vias = s.get_all("via"); // all three, in arrival order
```

A proxy walking the Via chain to detect loops or unwind a response path uses `get_all`. A simple application that just wants the next hop's branch tag uses `get_element`.

---

## Body framing

SIP forbids chunked transfer-encoding. The framing rule is:

1. CRLFCRLF not present → `message_length()` returns 0 (need more bytes).
2. `Transfer-Encoding: chunked` present → returns 0 **and** the constructor sets `error()` (protocol violation).
3. `Content-Length: N` (canonical or `l:`) → returns `header_len + N`.
4. Otherwise → returns `header_len` (no body).

The body is **opaque bytes**. SDP is not parsed at this layer; the test harness round-trips a representative SDP without mutation (`SipParser.HandlesSdpBody_Opaque`).

---

## Public API

```cpp
class Sip : public MessageParser {
public:
  Sip() = default;
  explicit Sip(const std::string& in);

  bool is_request() const;
  bool error()      const;

  const std::string& method()        const;  // request side
  const std::string& uri()           const;  // request side
  int                status_code()   const;  // status side
  const std::string& reason_phrase() const;  // status side

  void        add_element(std::string key, std::string value) override;
  std::string get_element(const std::string& key) const   override;
  std::vector<std::string> get_all(const std::string& key) const;

  static std::size_t message_length(const std::string& buf);
};
```

---

## Behaviour pinned by tests (`test/sip_parser_test.cc` — 17 tests)

Request line: `ParsesRequestLine_Invite`, `ParsesRequestLine_Register`.

Status line: `ParsesStatusLine_200Ok`, `ParsesStatusLine_486BusyHere`.

Discrimination regression: `DistinguishesRequestFromStatus`.

Compact aliases: `CompactHeader_LResolvesToContentLength` (also pins body framing via compact `l:`), `CompactHeader_VResolvesToVia`, `CompactHeader_AllAliases` (every alias in one shot).

Multi-value: `CanonicalAndCompact_Coexist` (mixed `Via:`/`v:` keeps order), `MultipleRecordRoute_PreservedInOrder`.

Framing: `MessageLength_UsesCompactContentLen`, `MessageLength_NeedsMore_NoSeparator`, `MessageLength_NoBody`.

Error paths: `ChunkedEncoding_Rejected`, `RejectsMalformedFirstLine_NoSpaces`, `RejectsMalformedFirstLine_MissingSipVersion`.

Body opacity: `HandlesSdpBody_Opaque`.

---

## Files

```
sip/
  inc/sip_parser.hpp
  src/sip_parser.cpp
  test/sip_parser_test.cc
```

## Dependencies

- `http/inc/message_parser.hpp` (base class)
- GTest (tests only)
- No ACE, no zlib

## Who depends on us

- [`pbx/`](../pbx/README.md) — `SipBridge` (Layer 1) optionally peeks the SIP first line for abuse filtering before forwarding the frame down the tunnel.
- Layer 4 SIP scenario tests — synthetic UAs build raw INVITE/REGISTER bytes and use `Sip` to parse Asterisk's responses.

## Build & test

```sh
podman run --rm --entrypoint ./offtarget onprem-pbx-test:layer0 \
  --gtest_filter='SipParser.*'
```

Expected: **17/17 PASSED**.

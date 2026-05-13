#include "sip_parser.hpp"
#include <gtest/gtest.h>

namespace {

const std::string CRLF = "\r\n";

std::string make_invite_with_l_compact()
{
    return
        "INVITE sip:b-204@society SIP/2.0" + CRLF +
        "Via: SIP/2.0/WSS proxy.example;branch=z9hG4bK1" + CRLF +
        "From: \"Alice\" <sip:a-101@society>;tag=abc" + CRLF +
        "To: <sip:b-204@society>" + CRLF +
        "Call-ID: 12345@society" + CRLF +
        "CSeq: 1 INVITE" + CRLF +
        "l: 12" + CRLF +
        "c: application/sdp" + CRLF +
        CRLF +
        "v=0\r\nm=audio";  // 12 bytes
}

} // namespace

// ── Request line ──────────────────────────────────────────────────────────────

TEST(SipParser, ParsesRequestLine_Invite)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_TRUE(s.is_request());
    EXPECT_EQ("INVITE", s.method());
    EXPECT_EQ("sip:b-204@society", s.uri());
}

TEST(SipParser, ParsesRequestLine_Register)
{
    const std::string msg =
        "REGISTER sip:society SIP/2.0\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_TRUE(s.is_request());
    EXPECT_EQ("REGISTER", s.method());
    EXPECT_EQ("sip:society", s.uri());
}

// ── Status line ───────────────────────────────────────────────────────────────

TEST(SipParser, ParsesStatusLine_200Ok)
{
    const std::string msg =
        "SIP/2.0 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_FALSE(s.is_request());
    EXPECT_EQ(200, s.status_code());
    EXPECT_EQ("OK", s.reason_phrase());
}

TEST(SipParser, ParsesStatusLine_486BusyHere)
{
    const std::string msg =
        "SIP/2.0 486 Busy Here\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_FALSE(s.is_request());
    EXPECT_EQ(486, s.status_code());
    EXPECT_EQ("Busy Here", s.reason_phrase());
}

TEST(SipParser, DistinguishesRequestFromStatus)
{
    // Regression for the "m_method = 'SIP/2.0'" bug from the naive base.
    const std::string status =
        "SIP/2.0 100 Trying\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(status);
    EXPECT_FALSE(s.is_request());
    EXPECT_TRUE(s.method().empty()); // method MUST NOT be "SIP/2.0"
}

// ── Compact headers ───────────────────────────────────────────────────────────

TEST(SipParser, CompactHeader_LResolvesToContentLength)
{
    Sip s(make_invite_with_l_compact());
    EXPECT_FALSE(s.error());
    EXPECT_EQ("12", s.get_element("Content-Length"));
    EXPECT_EQ("12", s.get_element("content-length"));
    EXPECT_EQ("12", s.get_element("l"));
    EXPECT_EQ("v=0\r\nm=audio", s.body());
}

TEST(SipParser, CompactHeader_VResolvesToVia)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "v: SIP/2.0/WSS edge1;branch=z9hG4bK1\r\n"
        "v: SIP/2.0/WSS edge2;branch=z9hG4bK2\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_EQ("SIP/2.0/WSS edge1;branch=z9hG4bK1", s.get_element("via"));
    auto vias = s.get_all("via");
    ASSERT_EQ(2u, vias.size());
    EXPECT_EQ("SIP/2.0/WSS edge1;branch=z9hG4bK1", vias[0]);
    EXPECT_EQ("SIP/2.0/WSS edge2;branch=z9hG4bK2", vias[1]);
}

TEST(SipParser, CompactHeader_AllAliases)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "i: call-id-1\r\n"
        "f: <sip:a@x>;tag=t1\r\n"
        "t: <sip:b@x>\r\n"
        "m: <sip:a@host>\r\n"
        "c: application/sdp\r\n"
        "s: subject-text\r\n"
        "k: replaces\r\n"
        "e: gzip\r\n"
        "o: presence\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_EQ("call-id-1",            s.get_element("call-id"));
    EXPECT_EQ("<sip:a@x>;tag=t1",     s.get_element("from"));
    EXPECT_EQ("<sip:b@x>",            s.get_element("to"));
    EXPECT_EQ("<sip:a@host>",         s.get_element("contact"));
    EXPECT_EQ("application/sdp",      s.get_element("content-type"));
    EXPECT_EQ("subject-text",         s.get_element("subject"));
    EXPECT_EQ("replaces",             s.get_element("supported"));
    EXPECT_EQ("gzip",                 s.get_element("content-encoding"));
    EXPECT_EQ("presence",             s.get_element("event"));
}

TEST(SipParser, CanonicalAndCompact_Coexist)
{
    // Mixed Via: and v: must preserve arrival order.
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Via: SIP/2.0/WSS hop1;branch=z9hG4bK1\r\n"
        "v: SIP/2.0/WSS hop2;branch=z9hG4bK2\r\n"
        "Via: SIP/2.0/WSS hop3;branch=z9hG4bK3\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    auto vias = s.get_all("via");
    ASSERT_EQ(3u, vias.size());
    EXPECT_EQ("SIP/2.0/WSS hop1;branch=z9hG4bK1", vias[0]);
    EXPECT_EQ("SIP/2.0/WSS hop2;branch=z9hG4bK2", vias[1]);
    EXPECT_EQ("SIP/2.0/WSS hop3;branch=z9hG4bK3", vias[2]);
    // Top-most (first) is what get_element returns.
    EXPECT_EQ("SIP/2.0/WSS hop1;branch=z9hG4bK1", s.get_element("Via"));
}

TEST(SipParser, MultipleRecordRoute_PreservedInOrder)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Record-Route: <sip:proxy1;lr>\r\n"
        "Record-Route: <sip:proxy2;lr>\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    Sip s(msg);
    auto rr = s.get_all("record-route");
    ASSERT_EQ(2u, rr.size());
    EXPECT_EQ("<sip:proxy1;lr>", rr[0]);
    EXPECT_EQ("<sip:proxy2;lr>", rr[1]);
}

// ── message_length ────────────────────────────────────────────────────────────

TEST(SipParser, MessageLength_UsesCompactContentLen)
{
    // Compact `l:` must drive body framing, not just canonical Content-Length.
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "l: 5\r\n"
        "\r\n"
        "hello";
    EXPECT_EQ(msg.size(), Sip::message_length(msg));
}

TEST(SipParser, MessageLength_NeedsMore_NoSeparator)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Content-Length: 5\r\n";
    EXPECT_EQ(0u, Sip::message_length(msg));
}

TEST(SipParser, MessageLength_NoBody)
{
    const std::string msg =
        "REGISTER sip:society SIP/2.0\r\n"
        "Via: SIP/2.0/WSS x\r\n"
        "\r\n";
    EXPECT_EQ(msg.size(), Sip::message_length(msg));
}

// ── SIP-forbidden body framing ────────────────────────────────────────────────

TEST(SipParser, ChunkedEncoding_Rejected)
{
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    Sip s(msg);
    EXPECT_TRUE(s.error());
    EXPECT_EQ(0u, Sip::message_length(msg));
}

// ── Malformed input ───────────────────────────────────────────────────────────

TEST(SipParser, RejectsMalformedFirstLine_NoSpaces)
{
    Sip s(std::string("nonsense\r\n\r\n"));
    EXPECT_TRUE(s.error());
}

TEST(SipParser, RejectsMalformedFirstLine_MissingSipVersion)
{
    Sip s(std::string("INVITE sip:x HTTP/1.1\r\n\r\n"));
    EXPECT_TRUE(s.error());
}

// ── SDP body opacity ──────────────────────────────────────────────────────────

TEST(SipParser, HandlesSdpBody_Opaque)
{
    const std::string sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "m=audio 49170 RTP/AVP 0\r\n";
    const std::string cl = std::to_string(sdp.size());
    const std::string msg =
        "INVITE sip:b-204@society SIP/2.0\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: " + cl + "\r\n"
        "\r\n" + sdp;

    Sip s(msg);
    EXPECT_FALSE(s.error());
    EXPECT_EQ(sdp, s.body());
}

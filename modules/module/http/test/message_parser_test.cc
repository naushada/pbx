#include "message_parser.hpp"
#include <gtest/gtest.h>

namespace {

// A minimal concrete subclass — we only want to exercise the base-class
// surface (extract_header, parse_mime_header, add_element, get_element).
class BareParser : public MessageParser {};

} // namespace

// ── extract_header ────────────────────────────────────────────────────────────

TEST(MessageParserBase, HeaderSeparator_Detected)
{
    const std::string in =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n"
        "body-bytes";

    const std::string header = MessageParser::extract_header(in);
    ASSERT_FALSE(header.empty());
    EXPECT_EQ(header.back(), '\n');
    EXPECT_EQ(header.substr(header.size() - 4), "\r\n\r\n");
    // The header section excludes the body.
    EXPECT_EQ(header.find("body-bytes"), std::string::npos);
}

TEST(MessageParserBase, HeaderSeparator_ReturnsWholeBufferWhenAbsent)
{
    const std::string in = "GET / HTTP/1.1\r\nHost: example.com\r\n"; // no CRLFCRLF yet
    EXPECT_EQ(MessageParser::extract_header(in), in);
}

// ── parse_mime_header ─────────────────────────────────────────────────────────

TEST(MessageParserBase, MimeHeader_LowercasedKeys)
{
    BareParser p;
    const std::string in =
        "FIRST-LINE-SKIPPED\r\n"
        "Host: example.com\r\n"
        "X-Custom-HEADER: value-as-is\r\n"
        "\r\n";

    p.parse_mime_header(in);
    EXPECT_EQ("example.com",  p.get_element("Host"));
    EXPECT_EQ("example.com",  p.get_element("HOST"));     // case-insensitive lookup
    EXPECT_EQ("value-as-is",  p.get_element("x-custom-header"));
}

TEST(MessageParserBase, MimeHeader_StopsAtBlankLine)
{
    BareParser p;
    const std::string in =
        "FIRST-LINE\r\n"
        "Host: example.com\r\n"
        "\r\n"
        "Bogus-Body-Header: should-not-parse\r\n";

    p.parse_mime_header(in);
    EXPECT_EQ("example.com", p.get_element("Host"));
    EXPECT_TRUE(p.get_element("Bogus-Body-Header").empty());
}

TEST(MessageParserBase, MimeHeader_SkipsLinesWithoutColon)
{
    BareParser p;
    const std::string in =
        "FIRST-LINE\r\n"
        "this-line-has-no-colon\r\n"
        "Host: example.com\r\n"
        "\r\n";

    p.parse_mime_header(in);
    EXPECT_EQ("example.com", p.get_element("Host"));
}

// ── add_element / get_element ─────────────────────────────────────────────────

TEST(MessageParserBase, AddElement_AndLookup)
{
    BareParser p;
    p.add_element("Content-Length", "42");
    EXPECT_EQ("42", p.get_element("content-length"));
    EXPECT_EQ("42", p.get_element("Content-Length"));
}

TEST(MessageParserBase, GetElement_MissingKeyReturnsEmpty)
{
    BareParser p;
    EXPECT_TRUE(p.get_element("nothing-here").empty());
}

// ── pct_decode ────────────────────────────────────────────────────────────────

TEST(MessageParserBase, PctDecode_KnownVectors)
{
    EXPECT_EQ("hello world", pct_decode("hello%20world"));
    EXPECT_EQ("hello world", pct_decode("hello+world"));
    EXPECT_EQ("/api/v1/x",   pct_decode("%2Fapi%2Fv1%2Fx"));
    EXPECT_EQ("no-change",   pct_decode("no-change"));
}

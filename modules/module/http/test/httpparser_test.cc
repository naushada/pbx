#include <sstream>
#include "httpparser_test.hpp"

// ── Default constructor ───────────────────────────────────────────────────────

TEST(HttpParser, DefaultConstructorYieldsEmptyFields)
{
    Http h;
    EXPECT_TRUE(h.uri().empty());
    EXPECT_TRUE(h.method().empty());
    EXPECT_TRUE(h.body().empty());
    EXPECT_TRUE(h.header().empty());
    EXPECT_TRUE(h.get_element("Host").empty());
}

// ── Method and URI extraction ─────────────────────────────────────────────────

TEST(HttpParser, GetRequestRootUri)
{
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Connection: Keep-Alive\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("GET", h.method());
    EXPECT_EQ("/",   h.uri());
    EXPECT_EQ("www.example.com", h.get_element("Host"));
    EXPECT_EQ("Keep-Alive",      h.get_element("Connection"));
    EXPECT_TRUE(h.body().empty());
}

TEST(HttpParser, PostMethodExtracted)
{
    std::string body = "{\"k\":\"v\"}";
    std::string req =
        "POST /api/v1/shipment HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    Http h(req);
    EXPECT_EQ("POST",              h.method());
    EXPECT_EQ("/api/v1/shipment",  h.uri());
    EXPECT_EQ(body,                h.body());
}

TEST(HttpParser, PutMethodExtracted)
{
    std::string req =
        "PUT /api/v1/inventory/1 HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("PUT", h.method());
    EXPECT_EQ("/api/v1/inventory/1", h.uri());
}

TEST(HttpParser, DeleteMethodExtracted)
{
    std::string req =
        "DELETE /api/v1/account/42 HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("DELETE", h.method());
    EXPECT_EQ("/api/v1/account/42", h.uri());
}

TEST(HttpParser, OptionsMethodExtracted)
{
    std::string req =
        "OPTIONS /api/v1/shipment HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("OPTIONS", h.method());
    EXPECT_EQ("/api/v1/shipment", h.uri());
}

// ── Query string parsing ──────────────────────────────────────────────────────

TEST(HttpParser, SingleQueryParam)
{
    std::string req =
        "GET /search?q=hello HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("/search", h.uri());
    EXPECT_EQ("hello",   h.get_element("q"));
}

TEST(HttpParser, MultipleQueryParams)
{
    std::string req =
        "GET /api?page=2&limit=50 HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("2",  h.get_element("page"));
    EXPECT_EQ("50", h.get_element("limit"));
}

TEST(HttpParser, PercentEncodedQueryParam)
{
    std::string req =
        "GET /search?q=hello%20world HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("hello world", h.get_element("q"));
}

TEST(HttpParser, PlusEncodedQueryParam)
{
    std::string req =
        "GET /search?q=hello+world HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("hello world", h.get_element("q"));
}

TEST(HttpParser, PercentEncodedUri)
{
    std::string req =
        "GET /path%2Fwith%2Fslashes HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_EQ("/path/with/slashes", h.uri());
}

TEST(HttpParser, MissingQueryParamReturnsEmpty)
{
    std::string req =
        "GET /api?foo=bar HTTP/1.1\r\n"
        "\r\n";

    Http h(req);
    EXPECT_TRUE(h.get_element("missing").empty());
}

// ── MIME header parsing ───────────────────────────────────────────────────────

TEST(HttpParser, MimeHeadersAllParsed)
{
    const std::string ua =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

    std::string body = "{\"key\":\"value\"}";
    std::string req =
        "POST /api/v1/shipment/shipping HTTP/1.1\r\n"
        "Host: www.dummyhost.com\r\n"
        "Connection: Keep-Alive\r\n"
        "Content-Type: application/json\r\n"
        "User-Agent: " + ua + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    Http h(req);
    EXPECT_EQ("www.dummyhost.com",   h.get_element("Host"));
    EXPECT_EQ("Keep-Alive",          h.get_element("Connection"));
    EXPECT_EQ("application/json",    h.get_element("Content-Type"));
    EXPECT_EQ(ua,                    h.get_element("User-Agent"));
    EXPECT_EQ(std::to_string(body.size()), h.get_element("Content-Length"));
}

// ── Body extraction via Content-Length ───────────────────────────────────────

TEST(HttpParser, BodyExtractedViaContentLength)
{
    std::string body = "{\"key1\": \"value1\", \"key2\": \"value2\"}";
    std::string req =
        "POST /api/v1/shipment/shipping HTTP/1.1\r\n"
        "Host: www.dummyhost.com\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    Http h(req);
    EXPECT_EQ(body, h.body());
    EXPECT_EQ(h.header().size() + h.body().size(), req.size());
}

TEST(HttpParser, EmptyBodyWhenNoContent)
{
    std::string req =
        "GET /api/v1/shipment HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "\r\n";

    Http h(req);
    EXPECT_TRUE(h.body().empty());
}

// ── Chunked transfer encoding ─────────────────────────────────────────────────

TEST(HttpParser, ChunkedBodyDecoded)
{
    const std::string payload = "Hello, chunked world!";
    std::string chunked = chunked_encode(payload);

    std::string req =
        "POST /api/v1/data HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n" + chunked;

    Http h(req);
    EXPECT_EQ(payload, h.body());
}

TEST(HttpParser, MultiChunkBodyDecoded)
{
    // Build a two-chunk body manually: "Hello" + " World"
    std::string chunked =
        "5\r\nHello\r\n"
        "6\r\n World\r\n"
        "0\r\n\r\n";

    std::string req =
        "POST /upload HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n" + chunked;

    Http h(req);
    EXPECT_EQ("Hello World", h.body());
}

// ── Gzip Content-Encoding ─────────────────────────────────────────────────────

TEST(HttpParser, GzipBodyDecompressed)
{
    const std::string payload = "{\"status\":\"ok\"}";
    std::string compressed = gzip_compress(payload);
    ASSERT_FALSE(compressed.empty());

    std::string req =
        "POST /api/v1/shipment HTTP/1.1\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Length: " + std::to_string(compressed.size()) + "\r\n"
        "\r\n" + compressed;

    Http h(req);
    EXPECT_EQ(payload, h.body());
}

TEST(HttpParser, GzipAndChunkedBodyDecoded)
{
    const std::string payload = "compressed and chunked payload";
    std::string compressed = gzip_compress(payload);
    ASSERT_FALSE(compressed.empty());

    std::string chunked = chunked_encode(compressed);

    std::string req =
        "POST /upload HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Encoding: gzip\r\n"
        "\r\n" + chunked;

    Http h(req);
    EXPECT_EQ(payload, h.body());
}

// ── Header boundary ───────────────────────────────────────────────────────────

TEST(HttpParser, HeaderIncludesTrailingCrLfCrLf)
{
    std::string body = "data";
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: x.com\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    Http h(req);
    // header() ends at and includes the blank line separator
    EXPECT_EQ("\r\n\r\n", h.header().substr(h.header().size() - 4));
    EXPECT_EQ(body, h.body());
}

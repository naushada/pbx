#include "ace_ssl_transport.hpp"
#include "wsframe.hpp"

#include <gtest/gtest.h>
#include <set>
#include <string>

// ── build_upgrade_request ────────────────────────────────────────────────────

TEST(AceSslTransport, BuildUpgradeRequest_HasRequiredHeaders)
{
    auto [req, key] =
        AceSslTransport::build_upgrade_request("agent.example.com", "/agent");

    EXPECT_NE(std::string::npos, req.find("GET /agent HTTP/1.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Host: agent.example.com\r\n"));
    EXPECT_NE(std::string::npos, req.find("Upgrade: websocket\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: Upgrade\r\n"));
    EXPECT_NE(std::string::npos, req.find("Sec-WebSocket-Version: 13\r\n"));
    EXPECT_NE(std::string::npos,
              req.find("Sec-WebSocket-Key: " + key + "\r\n"));

    // Ends with the required CRLFCRLF.
    EXPECT_EQ("\r\n\r\n", req.substr(req.size() - 4));
}

TEST(AceSslTransport, BuildUpgradeRequest_KeyIs24CharsBase64)
{
    auto [req, key] =
        AceSslTransport::build_upgrade_request("host", "/agent");

    // 16 raw bytes → 22 base64 chars + 2 '=' padding = 24 chars.
    EXPECT_EQ(24u, key.size());
    EXPECT_EQ('=', key[22]);
    EXPECT_EQ('=', key[23]);
}

TEST(AceSslTransport, BuildUpgradeRequest_GeneratesUniqueKeys)
{
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        auto [req, key] =
            AceSslTransport::build_upgrade_request("h", "/agent");
        EXPECT_TRUE(seen.insert(key).second)
            << "duplicate key generated: " << key;
    }
}

TEST(AceSslTransport, BuildUpgradeRequest_RespectsCustomPath)
{
    auto [req, key] =
        AceSslTransport::build_upgrade_request("h", "/non-standard/agent");
    EXPECT_NE(std::string::npos,
              req.find("GET /non-standard/agent HTTP/1.1\r\n"));
}

// ── validate_upgrade_response ────────────────────────────────────────────────

namespace {

std::string make_101_response(const std::string &accept) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

} // namespace

TEST(AceSslTransport, ValidateUpgradeResponse_AcceptsCorrect101)
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string expected = wsframe::accept_key(key);
    const std::string headers  = make_101_response(expected);

    EXPECT_TRUE(AceSslTransport::validate_upgrade_response(headers, key));
}

TEST(AceSslTransport, ValidateUpgradeResponse_RejectsNon101)
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string expected = wsframe::accept_key(key);
    const std::string headers =
        "HTTP/1.1 400 Bad Request\r\n"
        "Sec-WebSocket-Accept: " + expected + "\r\n\r\n";

    EXPECT_FALSE(AceSslTransport::validate_upgrade_response(headers, key));
}

TEST(AceSslTransport, ValidateUpgradeResponse_RejectsMissingAccept)
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string headers =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n\r\n";

    EXPECT_FALSE(AceSslTransport::validate_upgrade_response(headers, key));
}

TEST(AceSslTransport, ValidateUpgradeResponse_RejectsWrongAccept)
{
    const std::string key  = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string bad  = "AAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const std::string headers = make_101_response(bad);

    EXPECT_FALSE(AceSslTransport::validate_upgrade_response(headers, key));
}

TEST(AceSslTransport, ValidateUpgradeResponse_RejectsGarbage)
{
    const std::string key = "x";
    EXPECT_FALSE(AceSslTransport::validate_upgrade_response("", key));
    EXPECT_FALSE(AceSslTransport::validate_upgrade_response(
        "not even a status line\r\n\r\n", key));
}

// ── Factory smoke: unreachable host returns nullptr (no segfault) ────────────

TEST(AceSslTransport, FactoryUnreachableHost_ReturnsNullptr)
{
    AceSslTransportFactory fac(
        /*reactor=*/nullptr,
        /*on_bytes=*/[](const std::string &) {},
        /*on_disconnect=*/[]() {});

    // Localhost port 1 is reserved & not listening — connect must fail
    // cleanly within the 10-second timeout. We pass empty cert paths
    // because the TCP connect should fail before the TLS handshake even
    // begins.
    auto t = fac.create_connected("127.0.0.1", 1,
                                   /*cert=*/"", /*key=*/"", /*ca=*/"");
    EXPECT_EQ(nullptr, t);
}

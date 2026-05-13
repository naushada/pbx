#include "ace_https_client.hpp"

#include <gtest/gtest.h>
#include <string>

// ── parse_url ────────────────────────────────────────────────────────────────

TEST(AceHttpsClient, ParseUrl_TypicalPushEndpoint)
{
    auto u = AceHttpsClient::parse_url(
        "https://updates.push.services.mozilla.com/wpush/v2/abc-xyz");
    EXPECT_TRUE(u.ok);
    EXPECT_EQ("https",                                u.scheme);
    EXPECT_EQ("updates.push.services.mozilla.com",    u.host);
    EXPECT_EQ(443,                                    u.port);
    EXPECT_EQ("/wpush/v2/abc-xyz",                    u.path_and_query);
}

TEST(AceHttpsClient, ParseUrl_ExplicitPort)
{
    auto u = AceHttpsClient::parse_url("https://example.com:8443/p");
    EXPECT_TRUE(u.ok);
    EXPECT_EQ("example.com", u.host);
    EXPECT_EQ(8443,          u.port);
    EXPECT_EQ("/p",          u.path_and_query);
}

TEST(AceHttpsClient, ParseUrl_PreservesQueryAndFragment)
{
    auto u = AceHttpsClient::parse_url(
        "https://h/path?key=value&x=1#frag");
    EXPECT_TRUE(u.ok);
    EXPECT_EQ("/path?key=value&x=1#frag", u.path_and_query);
}

TEST(AceHttpsClient, ParseUrl_NoPath_DefaultsToSlash)
{
    auto u = AceHttpsClient::parse_url("https://example.com");
    EXPECT_TRUE(u.ok);
    EXPECT_EQ("example.com", u.host);
    EXPECT_EQ(443,           u.port);
    EXPECT_EQ("/",           u.path_and_query);
}

TEST(AceHttpsClient, ParseUrl_RejectsHttpScheme)
{
    auto u = AceHttpsClient::parse_url("http://example.com/p");
    EXPECT_FALSE(u.ok);
}

TEST(AceHttpsClient, ParseUrl_RejectsGarbage)
{
    EXPECT_FALSE(AceHttpsClient::parse_url("").ok);
    EXPECT_FALSE(AceHttpsClient::parse_url("https://").ok);
    EXPECT_FALSE(AceHttpsClient::parse_url("not a url at all").ok);
    EXPECT_FALSE(AceHttpsClient::parse_url("https://host:not-a-number/p").ok);
    EXPECT_FALSE(AceHttpsClient::parse_url("https://host:99999/p").ok);
}

// ── build_request ────────────────────────────────────────────────────────────

TEST(AceHttpsClient, BuildRequest_HasHostAndContentLengthAndConnectionClose)
{
    const std::string req = AceHttpsClient::build_request(
        "example.com", "/p",
        {{"Authorization", "vapid t=...,k=..."},
         {"Content-Encoding", "aes128gcm"}},
        "binary-body-bytes");

    EXPECT_NE(std::string::npos, req.find("POST /p HTTP/1.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Host: example.com\r\n"));
    EXPECT_NE(std::string::npos,
              req.find("Authorization: vapid t=...,k=...\r\n"));
    EXPECT_NE(std::string::npos,
              req.find("Content-Encoding: aes128gcm\r\n"));
    EXPECT_NE(std::string::npos, req.find("Content-Length: 17\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: close\r\n"));
    // Body separated by CRLFCRLF.
    EXPECT_NE(std::string::npos, req.find("\r\n\r\nbinary-body-bytes"));
}

TEST(AceHttpsClient, BuildRequest_StripsCallerProvidedHostAndContentLength)
{
    // Caller shouldn't set Host / Content-Length / Connection — we control
    // those. Make sure we don't double-emit if they sneak in.
    const std::string req = AceHttpsClient::build_request(
        "real-host.example", "/p",
        {{"Host", "evil-host.example"},
         {"Content-Length", "999"},
         {"Connection", "keep-alive"},
         {"TTL", "60"}},
        "body");

    EXPECT_NE(std::string::npos, req.find("Host: real-host.example\r\n"));
    EXPECT_EQ(std::string::npos, req.find("Host: evil-host.example"));
    EXPECT_EQ(std::string::npos, req.find("Content-Length: 999"));
    EXPECT_NE(std::string::npos, req.find("Content-Length: 4\r\n"));
    EXPECT_EQ(std::string::npos, req.find("Connection: keep-alive"));
    EXPECT_NE(std::string::npos, req.find("Connection: close\r\n"));
    EXPECT_NE(std::string::npos, req.find("TTL: 60\r\n"));
}

// ── parse_response ───────────────────────────────────────────────────────────

TEST(AceHttpsClient, ParseResponse_201Created)
{
    const std::string raw =
        "HTTP/1.1 201 Created\r\n"
        "Location: https://push/endpoint/abc\r\n"
        "Content-Length: 0\r\n\r\n";
    auto r = AceHttpsClient::parse_response(raw);
    EXPECT_EQ(201, r.status);
    EXPECT_TRUE(r.body.empty());
}

TEST(AceHttpsClient, ParseResponse_410Gone)
{
    const std::string raw =
        "HTTP/1.1 410 Gone\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 4\r\n\r\n"
        "gone";
    auto r = AceHttpsClient::parse_response(raw);
    EXPECT_EQ(410,    r.status);
    EXPECT_EQ("gone", r.body);
}

TEST(AceHttpsClient, ParseResponse_503ServiceUnavailable)
{
    const std::string raw =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Retry-After: 60\r\n\r\n";
    auto r = AceHttpsClient::parse_response(raw);
    EXPECT_EQ(503, r.status);
}

TEST(AceHttpsClient, ParseResponse_Garbage_StatusZero)
{
    EXPECT_EQ(0, AceHttpsClient::parse_response("").status);
    EXPECT_EQ(0, AceHttpsClient::parse_response("not a response").status);
    EXPECT_EQ(0,
              AceHttpsClient::parse_response("HTTP/9.9 zzz x\r\n\r\n").status);
}

// ── Smoke: unreachable host → status 0 ───────────────────────────────────────

TEST(AceHttpsClient, UnreachableHost_ReturnsStatusZero)
{
    AceHttpsClient client;
    // Use a TCP port that's almost never listening on a local box. The
    // SSL connect will fail at the TCP-handshake stage well within the
    // 10s timeout — we just want to verify the failure path returns
    // cleanly (no segfault, no exception leak).
    auto r = client.post("https://127.0.0.1:1/anywhere", {}, "body");
    EXPECT_EQ(0, r.status);
    EXPECT_TRUE(r.body.empty());
}

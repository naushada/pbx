#include "ari_rest_client.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

// ── url_encode ────────────────────────────────────────────────────────────────

TEST(AriRestClient, UrlEncode_LeavesUnreservedAlone)
{
    EXPECT_EQ("abcXYZ012-_.~",
              AriRestClient::url_encode("abcXYZ012-_.~"));
}

TEST(AriRestClient, UrlEncode_EncodesReservedChars)
{
    EXPECT_EQ("foo%20bar", AriRestClient::url_encode("foo bar"));
    EXPECT_EQ("a%2Fb",     AriRestClient::url_encode("a/b"));
    EXPECT_EQ("k%3Dv",     AriRestClient::url_encode("k=v"));
    EXPECT_EQ("a%3Ab",     AriRestClient::url_encode("a:b"));
    EXPECT_EQ("%3F%26",    AriRestClient::url_encode("?&"));
}

TEST(AriRestClient, UrlEncode_EncodesNonAscii)
{
    // 'é' is 0xC3 0xA9 in UTF-8.
    const std::string raw   = "\xC3\xA9";
    const std::string enc   = AriRestClient::url_encode(raw);
    EXPECT_EQ("%C3%A9", enc);
}

// ── base64_encode ─────────────────────────────────────────────────────────────

TEST(AriRestClient, Base64Encode_KnownVectors)
{
    EXPECT_EQ("",         AriRestClient::base64_encode(""));
    EXPECT_EQ("Zg==",     AriRestClient::base64_encode("f"));
    EXPECT_EQ("Zm8=",     AriRestClient::base64_encode("fo"));
    EXPECT_EQ("Zm9v",     AriRestClient::base64_encode("foo"));
    EXPECT_EQ("YXN0ZXJpc2s6YXN0ZXJpc2s=",
              AriRestClient::base64_encode("asterisk:asterisk"));
}

// ── build_subscribe_request ───────────────────────────────────────────────────

TEST(AriRestClient, BuildSubscribeRequest_PathAndQuery)
{
    const std::string req = AriRestClient::build_subscribe_request(
        "pbx", {"channel:", "bridge:"}, "127.0.0.1", "YWJj");

    EXPECT_NE(std::string::npos,
              req.find("POST /ari/applications/pbx/subscription"
                       "?eventSource=channel%3A&eventSource=bridge%3A "
                       "HTTP/1.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Host: 127.0.0.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Authorization: Basic YWJj\r\n"));
    EXPECT_NE(std::string::npos, req.find("Content-Length: 0\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: close\r\n"));
    EXPECT_EQ("\r\n\r\n", req.substr(req.size() - 4));
}

TEST(AriRestClient, BuildSubscribeRequest_EscapesAppNameInPath)
{
    const std::string req = AriRestClient::build_subscribe_request(
        "my pbx", {"channel:"}, "h", "YWJj");
    EXPECT_NE(std::string::npos, req.find("/ari/applications/my%20pbx/"));
}

TEST(AriRestClient, BuildSubscribeRequest_NoSources_EmptyQueryString)
{
    const std::string req = AriRestClient::build_subscribe_request(
        "pbx", {}, "h", "YWJj");
    EXPECT_NE(std::string::npos,
              req.find("POST /ari/applications/pbx/subscription? HTTP/1.1"));
}

// ── build_continue_request ────────────────────────────────────────────────────

TEST(AriRestClient, BuildContinueRequest_PathAndQuery)
{
    const std::string req = AriRestClient::build_continue_request(
        "1700000000.42", "pbx-busy", "s", 1, "127.0.0.1", "YWJj");

    EXPECT_NE(std::string::npos,
              req.find("POST /ari/channels/1700000000.42/continue"
                       "?context=pbx-busy&extension=s&priority=1 "
                       "HTTP/1.1\r\n"));
    EXPECT_NE(std::string::npos, req.find("Authorization: Basic YWJj\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: close\r\n"));
}

TEST(AriRestClient, BuildContinueRequest_EscapesChannelId)
{
    // Channel IDs from Asterisk usually look like "1700000000.42", but if
    // someone passes a slash-bearing identifier it must be percent-encoded
    // so we hit the right REST path, not a sub-resource.
    const std::string req = AriRestClient::build_continue_request(
        "weird/cid", "ctx", "ext", 2, "h", "YWJj");
    EXPECT_NE(std::string::npos,
              req.find("/ari/channels/weird%2Fcid/continue"));
}

// ── parse_response ────────────────────────────────────────────────────────────

TEST(AriRestClient, ParseResponse_204NoContent)
{
    const std::string raw =
        "HTTP/1.1 204 No Content\r\n"
        "Server: Asterisk\r\n"
        "Content-Length: 0\r\n\r\n";
    auto r = AriRestClient::parse_response(raw);
    EXPECT_EQ(204, r.status);
    EXPECT_TRUE(r.body.empty());
}

TEST(AriRestClient, ParseResponse_404WithJson)
{
    const std::string raw =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 31\r\n\r\n"
        "{\"message\":\"Channel not found\"}";
    auto r = AriRestClient::parse_response(raw);
    EXPECT_EQ(404, r.status);
    EXPECT_EQ("{\"message\":\"Channel not found\"}", r.body);
}

TEST(AriRestClient, ParseResponse_Garbage_StatusZero)
{
    EXPECT_EQ(0, AriRestClient::parse_response("").status);
    EXPECT_EQ(0, AriRestClient::parse_response("not a response").status);
    EXPECT_EQ(0, AriRestClient::parse_response("HTTP/9.9 zzz xx\r\n\r\n").status);
}

TEST(AriRestClient, ParseResponse_NoBody_StatusOnly)
{
    const std::string raw = "HTTP/1.1 200 OK\r\nServer: x\r\n";
    auto r = AriRestClient::parse_response(raw);
    // CRLFCRLF not present → no body extracted, but status line parsed.
    EXPECT_EQ(200, r.status);
    EXPECT_TRUE(r.body.empty());
}

// ── Smoke: unreachable host returns status 0 ──────────────────────────────────

TEST(AriRestClient, UnreachableHost_ReturnsStatusZero)
{
    AriRestClient::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1; // not listening
    AriRestClient client(cfg);

    auto r = client.subscribe("pbx", {"channel:", "bridge:"});
    EXPECT_EQ(0, r.status);
    EXPECT_TRUE(r.body.empty());

    r = client.continue_in_dialplan("ch", "pbx-busy", "s", 1);
    EXPECT_EQ(0, r.status);
}

#include "ari_ws_client.hpp"
#include "wsframe.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> ws_text_masked(const std::string &payload) {
  return wsframe::encode(std::vector<std::uint8_t>(payload.begin(),
                                                     payload.end()),
                          /*opcode=*/0x1, /*mask=*/true);
}
std::vector<std::uint8_t> ws_ping_masked() {
  return wsframe::encode({}, /*opcode=*/0x9, /*mask=*/true);
}
std::vector<std::uint8_t> ws_close_masked() {
  return wsframe::encode({}, /*opcode=*/0x8, /*mask=*/true);
}

void make_socketpair(int fds[2]) {
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
      << "socketpair failed: " << std::strerror(errno);
}

std::vector<std::uint8_t> drain_socket(int fd) {
  std::vector<std::uint8_t> out;
  std::uint8_t buf[4096];
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0) break;
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

} // namespace

// ── base64_encode ────────────────────────────────────────────────────────────

TEST(AriWsClient, Base64Encode_KnownVectors)
{
    // RFC 4648 §10 test vectors.
    EXPECT_EQ("",         AriWsClient::base64_encode(""));
    EXPECT_EQ("Zg==",     AriWsClient::base64_encode("f"));
    EXPECT_EQ("Zm8=",     AriWsClient::base64_encode("fo"));
    EXPECT_EQ("Zm9v",     AriWsClient::base64_encode("foo"));
    EXPECT_EQ("Zm9vYg==", AriWsClient::base64_encode("foob"));
    EXPECT_EQ("Zm9vYmE=", AriWsClient::base64_encode("fooba"));
    EXPECT_EQ("Zm9vYmFy", AriWsClient::base64_encode("foobar"));
}

TEST(AriWsClient, Base64Encode_BasicAuthString)
{
    // "asterisk:asterisk" → standard base64 of those 17 bytes.
    const std::string expected = "YXN0ZXJpc2s6YXN0ZXJpc2s=";
    EXPECT_EQ(expected, AriWsClient::base64_encode("asterisk:asterisk"));
}

// ── build_upgrade_request ────────────────────────────────────────────────────

TEST(AriWsClient, BuildUpgradeRequest_IncludesAppParam_AndSubscribeAll)
{
    auto [req, key] = AriWsClient::build_upgrade_request(
        "127.0.0.1", "pbx", "asterisk", "asterisk");
    EXPECT_NE(std::string::npos,
              req.find("GET /ari/events?app=pbx&subscribeAll=true HTTP/1.1\r\n"))
        << "subscribeAll=true is REQUIRED for the app to receive events "
        << "for resources it doesn't explicitly own (endpoint state, "
        << "dialplan-entered StasisStart). Without it the WS stays "
        << "silent — Asterisk publishes into the void.";
    EXPECT_NE(std::string::npos, req.find("Host: 127.0.0.1\r\n"));
}

TEST(AriWsClient, BuildUpgradeRequest_IncludesBasicAuth)
{
    auto [req, key] = AriWsClient::build_upgrade_request(
        "host", "pbx", "asterisk", "asterisk");
    EXPECT_NE(std::string::npos,
              req.find("Authorization: Basic YXN0ZXJpc2s6YXN0ZXJpc2s=\r\n"));
}

TEST(AriWsClient, BuildUpgradeRequest_HasRequiredWsHeaders)
{
    auto [req, key] = AriWsClient::build_upgrade_request(
        "h", "pbx", "u", "p");
    EXPECT_NE(std::string::npos, req.find("Upgrade: websocket\r\n"));
    EXPECT_NE(std::string::npos, req.find("Connection: Upgrade\r\n"));
    EXPECT_NE(std::string::npos, req.find("Sec-WebSocket-Version: 13\r\n"));
    EXPECT_NE(std::string::npos,
              req.find("Sec-WebSocket-Key: " + key + "\r\n"));
    EXPECT_EQ("\r\n\r\n", req.substr(req.size() - 4));
}

TEST(AriWsClient, BuildUpgradeRequest_GeneratesUniqueKeys)
{
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        auto [req, key] = AriWsClient::build_upgrade_request(
            "h", "pbx", "u", "p");
        EXPECT_TRUE(seen.insert(key).second);
    }
}

// ── validate_upgrade_response ────────────────────────────────────────────────

TEST(AriWsClient, ValidateUpgradeResponse_AcceptsCorrect101)
{
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string expected = wsframe::accept_key(key);
    const std::string headers =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + expected + "\r\n\r\n";
    EXPECT_TRUE(AriWsClient::validate_upgrade_response(headers, key));
}

TEST(AriWsClient, ValidateUpgradeResponse_Rejects401)
{
    const std::string key = "x";
    const std::string headers =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"asterisk\"\r\n\r\n";
    EXPECT_FALSE(AriWsClient::validate_upgrade_response(headers, key));
}

// ── On-the-wire dispatch via socketpair ──────────────────────────────────────

TEST(AriWsClient, OnInput_TextFrame_DispatchesToOnEvent)
{
    std::vector<std::string> events;
    int fds[2]; make_socketpair(fds);
    auto *c = new AriWsClient(fds[1],
        [&events](const std::string &s) { events.push_back(s); });

    const std::string ari_event =
        R"({"type":"StasisStart","channel":{"id":"ch-1"}})";
    const auto frame = ws_text_masked(ari_event);
    ::send(fds[0], frame.data(), frame.size(), 0);

    EXPECT_EQ(0, c->handle_input(c->get_handle()));
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ(ari_event, events[0]);

    delete c;
    ::close(fds[0]);
}

TEST(AriWsClient, OnInput_PingTriggersPong)
{
    int fds[2]; make_socketpair(fds);
    auto *c = new AriWsClient(fds[1], [](const std::string &) {});

    const auto ping = ws_ping_masked();
    ::send(fds[0], ping.data(), ping.size(), 0);
    EXPECT_EQ(0, c->handle_input(c->get_handle()));

    const auto reply = drain_socket(fds[0]);
    ASSERT_GE(reply.size(), 2u);
    EXPECT_EQ(0x8A, reply[0]); // FIN=1, opcode=0xA (pong), unmasked
    delete c;
    ::close(fds[0]);
}

TEST(AriWsClient, OnInput_CloseFrame_DisconnectsClient)
{
    bool disconnected = false;
    int fds[2]; make_socketpair(fds);
    auto *c = new AriWsClient(fds[1],
        [](const std::string &) {},
        [&disconnected]() { disconnected = true; });

    const auto cls = ws_close_masked();
    ::send(fds[0], cls.data(), cls.size(), 0);
    EXPECT_EQ(-1, c->handle_input(c->get_handle()));

    // The disconnect callback is the source of truth — `connected()`
    // becomes false only after the reactor calls handle_close (which
    // closes the fd). In production that's automatic when handle_input
    // returns -1; in this test we just verify the protocol contract.
    EXPECT_TRUE(disconnected);
    delete c;
    ::close(fds[0]);
}

TEST(AriWsClient, MultipleFramesInOneRead)
{
    std::vector<std::string> events;
    int fds[2]; make_socketpair(fds);
    auto *c = new AriWsClient(fds[1],
        [&events](const std::string &s) { events.push_back(s); });

    const auto a = ws_text_masked(R"({"type":"BridgeCreated"})");
    const auto b = ws_text_masked(R"({"type":"BridgeDestroyed"})");
    std::vector<std::uint8_t> blob;
    blob.insert(blob.end(), a.begin(), a.end());
    blob.insert(blob.end(), b.begin(), b.end());
    ::send(fds[0], blob.data(), blob.size(), 0);

    EXPECT_EQ(0, c->handle_input(c->get_handle()));
    ASSERT_EQ(2u, events.size());
    EXPECT_NE(std::string::npos, events[0].find("BridgeCreated"));
    EXPECT_NE(std::string::npos, events[1].find("BridgeDestroyed"));
    delete c;
    ::close(fds[0]);
}

TEST(AriWsClient, PartialFrameAcrossReads)
{
    std::vector<std::string> events;
    int fds[2]; make_socketpair(fds);
    auto *c = new AriWsClient(fds[1],
        [&events](const std::string &s) { events.push_back(s); });

    const auto frame = ws_text_masked(R"({"type":"ChannelDestroyed"})");
    const std::size_t split = 5;
    ASSERT_GT(frame.size(), split);

    ::send(fds[0], frame.data(), split, 0);
    EXPECT_EQ(0, c->handle_input(c->get_handle()));
    EXPECT_EQ(0u, events.size());

    ::send(fds[0], frame.data() + split, frame.size() - split, 0);
    EXPECT_EQ(0, c->handle_input(c->get_handle()));
    ASSERT_EQ(1u, events.size());
    EXPECT_NE(std::string::npos, events[0].find("ChannelDestroyed"));

    delete c;
    ::close(fds[0]);
}

// ── Connect against an unreachable Asterisk returns false ────────────────────

TEST(AriWsClient, ConnectUnreachable_ReturnsFalse)
{
    AriWsClient::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 1;  // not listening
    cfg.app_name = "pbx";
    cfg.username = "asterisk";
    cfg.password = "asterisk";

    AriWsClient c(cfg, [](const std::string &) {});
    EXPECT_FALSE(c.connect_and_handshake());
    EXPECT_FALSE(c.connected());
}

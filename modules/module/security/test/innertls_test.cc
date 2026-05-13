#include "innertls.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

// ═══════════════════════════════════════════════════════════════════════════════
// MockTransport — test double for ITransport
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

class MockTransport : public ITransport {
public:
  // Data queued for the next recv() call
  std::vector<std::uint8_t> recvBuffer;

  // All data sent via this transport (appended per send() call)
  std::vector<std::uint8_t> sentData;

  // If true, recv() returns false (simulating a disconnected peer)
  bool recvFails = false;

  // If true, send() returns false
  bool sendFails = false;

  bool send(const std::vector<std::uint8_t> &data) override {
    if (sendFails) return false;
    sentData.insert(sentData.end(), data.begin(), data.end());
    return true;
  }

  bool recv(std::vector<std::uint8_t> &data) override {
    if (recvFails) return false;
    if (recvBuffer.empty()) {
      // Nothing available — peer hasn't sent yet in this test step.
      // Return empty data (caller will need to retry after peer flushes).
      data.clear();
      return true;
    }
    data = std::move(recvBuffer);
    recvBuffer.clear();
    return true;
  }
};

// Reads a file from /src/certs/ (generated during Docker build).
std::string read_cert(const std::string &filename) {
  std::ifstream f("/src/certs/" + filename);
  if (!f.is_open()) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Inner TLS tests — all RED until innertls.cpp is implemented
// ═══════════════════════════════════════════════════════════════════════════════

class InnerTlsTest : public ::testing::Test {
protected:
  std::string server_cert = read_cert("server.crt");
  std::string server_key  = read_cert("server.key");

  void SetUp() override {
    if (server_cert.empty() || server_key.empty())
      GTEST_SKIP() << "Test certs not found in /src/certs/";
  }
};

TEST_F(InnerTlsTest, Handshake_CompletesSuccessfully)
{
    MockTransport client_transport;
    MockTransport server_transport;

    // Wire transports back-to-back: what client sends, server receives
    // This is done manually in the test — after client.flush, copy sent
    // bytes to server's recvBuffer and vice versa.
    auto exchange = [&]() {
        if (!client_transport.sentData.empty()) {
            server_transport.recvBuffer.insert(
                server_transport.recvBuffer.end(),
                client_transport.sentData.begin(),
                client_transport.sentData.end());
            client_transport.sentData.clear();
        }
        if (!server_transport.sentData.empty()) {
            client_transport.recvBuffer.insert(
                client_transport.recvBuffer.end(),
                server_transport.sentData.begin(),
                server_transport.sentData.end());
            server_transport.sentData.clear();
        }
    };

    InnerTlsClient client(client_transport);
    InnerTlsServer server(server_transport,
                          "/src/certs/server.crt",
                          "/src/certs/server.key");

    // Interleaved handshake
    bool client_done = false, server_done = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!client_done) client_done = client.handshake();
        exchange();
        if (!server_done) server_done = server.accept();
        if (client_done && server_done) break;
    }

    EXPECT_TRUE(client_done) << "Client handshake should complete";
    EXPECT_TRUE(server_done)  << "Server handshake should complete";
}

TEST_F(InnerTlsTest, Handshake_Fails_ReturnsFalse)
{
    // Client without server — handshake should fail
    MockTransport dead_transport;
    dead_transport.recvFails = true;

    InnerTlsClient client(dead_transport);
    EXPECT_FALSE(client.handshake());
}

TEST_F(InnerTlsTest, EncryptedData_DiffersFromPlaintext)
{
    MockTransport ct, st;
    auto exchange = [&]() {
        if (!ct.sentData.empty()) {
            st.recvBuffer.insert(st.recvBuffer.end(),
                                 ct.sentData.begin(), ct.sentData.end());
            ct.sentData.clear();
        }
        if (!st.sentData.empty()) {
            ct.recvBuffer.insert(ct.recvBuffer.end(),
                                 st.sentData.begin(), st.sentData.end());
            st.sentData.clear();
        }
    };

    InnerTlsClient client(ct);
    InnerTlsServer server(st, "/src/certs/server.crt",
                          "/src/certs/server.key");

    // Complete handshake
    bool cd = false, sd = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!cd) cd = client.handshake();
        exchange();
        if (!sd) sd = server.accept();
        if (cd && sd) break;
    }
    ASSERT_TRUE(cd && sd) << "Handshake must complete for this test";

    // Send plaintext
    ct.sentData.clear();
    std::vector<std::uint8_t> plain = {'H', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(client.send(plain));
    exchange();

    // The bytes on the wire must NOT contain "Hello"
    std::string wire(st.sentData.begin(), st.sentData.end());
    EXPECT_EQ(std::string::npos, wire.find("Hello"))
        << "Data on the wire must be encrypted";
}

TEST_F(InnerTlsTest, Roundtrip_LargePayload)
{
    MockTransport ct, st;
    auto exchange = [&]() {
        if (!ct.sentData.empty()) {
            st.recvBuffer.insert(st.recvBuffer.end(),
                                 ct.sentData.begin(), ct.sentData.end());
            ct.sentData.clear();
        }
        if (!st.sentData.empty()) {
            ct.recvBuffer.insert(ct.recvBuffer.end(),
                                 st.sentData.begin(), st.sentData.end());
            st.sentData.clear();
        }
    };

    InnerTlsClient client(ct);
    InnerTlsServer server(st, "/src/certs/server.crt",
                          "/src/certs/server.key");

    bool cd = false, sd = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!cd) cd = client.handshake();
        exchange();
        if (!sd) sd = server.accept();
        if (cd && sd) break;
    }
    ASSERT_TRUE(cd && sd);

    // Send 64 KB of data
    std::vector<std::uint8_t> big_data(64 * 1024);
    for (std::size_t i = 0; i < big_data.size(); ++i)
        big_data[i] = static_cast<std::uint8_t>(i & 0xFF);

    ASSERT_TRUE(client.send(big_data));
    exchange();

    // Server receives the encrypted data and decrypts
    std::vector<std::uint8_t> received;
    // Multiple recv calls may be needed due to TLS record fragmentation
    while (received.size() < big_data.size()) {
        std::vector<std::uint8_t> chunk;
        if (!server.recv(chunk)) break;
        if (chunk.empty()) break;
        received.insert(received.end(), chunk.begin(), chunk.end());
    }

    EXPECT_EQ(big_data, received)
        << "Large payload must roundtrip correctly";
}

// Regression: a single SSL_write of >16 KB plaintext produces multiple TLS
// records but one transport frame. recv() must return the *whole* plaintext
// in a single call — wsdbproxy/wsdbagent rely on this because they treat one
// inner-TLS message as one BSON document and parse it eagerly. Previously
// SSL_read was called once with a 16 KB buffer, silently truncating the
// response; the BSON parser then read default field values (reqid=-1) and
// the response was dropped, causing H12 timeouts on Heroku.
TEST_F(InnerTlsTest, Recv_ReturnsFullPlaintext_InSingleCall)
{
    MockTransport ct, st;
    auto exchange = [&]() {
        if (!ct.sentData.empty()) {
            st.recvBuffer.insert(st.recvBuffer.end(),
                                 ct.sentData.begin(), ct.sentData.end());
            ct.sentData.clear();
        }
        if (!st.sentData.empty()) {
            ct.recvBuffer.insert(ct.recvBuffer.end(),
                                 st.sentData.begin(), st.sentData.end());
            st.sentData.clear();
        }
    };

    InnerTlsClient client(ct);
    InnerTlsServer server(st, "/src/certs/server.crt",
                          "/src/certs/server.key");

    bool cd = false, sd = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!cd) cd = client.handshake();
        exchange();
        if (!sd) sd = server.accept();
        if (cd && sd) break;
    }
    ASSERT_TRUE(cd && sd);

    // 64 KB → ~4 TLS records, all in one transport frame.
    std::vector<std::uint8_t> big_data(64 * 1024);
    for (std::size_t i = 0; i < big_data.size(); ++i)
        big_data[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);

    ASSERT_TRUE(client.send(big_data));
    exchange();

    std::vector<std::uint8_t> received;
    ASSERT_TRUE(server.recv(received));
    EXPECT_EQ(received.size(), big_data.size())
        << "recv() must return the entire plaintext from a single peer send "
        << "in one call; otherwise callers that treat one recv as one "
        << "application message will parse truncated data.";
    EXPECT_EQ(received, big_data);

    // Same direction back: server → client.
    ASSERT_TRUE(server.send(big_data));
    exchange();

    std::vector<std::uint8_t> received2;
    ASSERT_TRUE(client.recv(received2));
    EXPECT_EQ(received2.size(), big_data.size());
    EXPECT_EQ(received2, big_data);
}

TEST_F(InnerTlsTest, MultipleMessages_StayInOrder)
{
    MockTransport ct, st;
    auto exchange = [&]() {
        if (!ct.sentData.empty()) {
            st.recvBuffer.insert(st.recvBuffer.end(),
                                 ct.sentData.begin(), ct.sentData.end());
            ct.sentData.clear();
        }
        if (!st.sentData.empty()) {
            ct.recvBuffer.insert(ct.recvBuffer.end(),
                                 st.sentData.begin(), st.sentData.end());
            st.sentData.clear();
        }
    };

    InnerTlsClient client(ct);
    InnerTlsServer server(st, "/src/certs/server.crt",
                          "/src/certs/server.key");

    bool cd = false, sd = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!cd) cd = client.handshake();
        exchange();
        if (!sd) sd = server.accept();
        if (cd && sd) break;
    }
    ASSERT_TRUE(cd && sd);

    // Send 5 messages sequentially
    for (int i = 0; i < 5; ++i) {
        std::vector<std::uint8_t> msg = {
            static_cast<std::uint8_t>('A' + i),
            static_cast<std::uint8_t>('0' + i)};
        ASSERT_TRUE(client.send(msg));
        exchange();

        std::vector<std::uint8_t> recv;
        ASSERT_TRUE(server.recv(recv));
        EXPECT_EQ(msg, recv) << "Message " << i << " mismatch";
    }
}

TEST_F(InnerTlsTest, Mitm_PlaintextInsteadOfTls_DetectedByClient)
{
    MockTransport transport;
    // Feed plaintext "Hello" instead of TLS ServerHello
    transport.recvBuffer = {'H', 'e', 'l', 'l', 'o'};

    InnerTlsClient client(transport);
    EXPECT_FALSE(client.handshake())
        << "Client must reject non-TLS data during handshake";
}

TEST_F(InnerTlsTest, Mitm_TamperedFrame_Detected)
{
    MockTransport ct, st;
    auto exchange = [&]() {
        if (!ct.sentData.empty()) {
            st.recvBuffer.insert(st.recvBuffer.end(),
                                 ct.sentData.begin(), ct.sentData.end());
            ct.sentData.clear();
        }
        if (!st.sentData.empty()) {
            ct.recvBuffer.insert(ct.recvBuffer.end(),
                                 st.sentData.begin(), st.sentData.end());
            st.sentData.clear();
        }
    };

    InnerTlsClient client(ct);
    InnerTlsServer server(st, "/src/certs/server.crt",
                          "/src/certs/server.key");

    bool cd = false, sd = false;
    for (int i = 0; i < 20; ++i) {
        exchange();
        if (!cd) cd = client.handshake();
        exchange();
        if (!sd) sd = server.accept();
        if (cd && sd) break;
    }
    ASSERT_TRUE(cd && sd);

    // Client sends a message
    ct.sentData.clear();
    ASSERT_TRUE(client.send({'s', 'e', 'c', 'r', 'e', 't'}));

    // Tamper one byte before delivering to server
    if (!ct.sentData.empty()) {
        ct.sentData[0] ^= 0xFF;  // flip bits in first byte
        st.recvBuffer = ct.sentData;
        ct.sentData.clear();
    }

    std::vector<std::uint8_t> recv;
    // Decryption should fail on tampered data
    EXPECT_FALSE(server.recv(recv))
        << "Tampered TLS frame must be detected (decryption failure)";
}

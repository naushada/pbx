#include "push_sender.hpp"
#include "json.hpp"
#include "mongodbc.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

// Fake HTTP client: scriptable per-attempt responses, records every POST.
class FakeHttpClient : public IPushHttpClient {
public:
  std::vector<Response> scripted;   // popped front per call
  std::size_t calls = 0;

  struct Captured {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
  };
  std::vector<Captured> captured;

  Response post(const std::string &url,
                const std::vector<std::pair<std::string, std::string>> &headers,
                const std::string &body) override {
    captured.push_back({url, headers, body});
    ++calls;
    if (scripted.empty()) return {200, ""};
    Response r = scripted.front();
    scripted.erase(scripted.begin());
    return r;
  }
};

class FixedClock : public IClock {
public:
  std::int64_t t;
  explicit FixedClock(std::int64_t now) : t(now) {}
  std::int64_t now_unix() const override { return t; }
};

// Minimal IMongodbClient that returns a canned subscription list and records
// delete_document calls.
class TestDb : public IMongodbClient {
public:
  std::string subs_json;
  std::vector<std::string> deleted_filters;

  const std::string &get_database() const override { return m_db; }

  std::string get_document(const std::string &, const std::string &,
                           const std::string &) override { return {}; }
  std::string create_document(const std::string &, const std::string &,
                              const std::string &) override { return {}; }
  std::int32_t create_bulk_document(const std::string &, const std::string &,
                                    const std::string &) override { return 0; }
  bool update_collection(const std::string &, const std::string &,
                         const std::string &) override { return false; }
  std::int32_t update_bulk_document(const std::string &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &) override {
    return 0;
  }
  bool delete_document(const std::string &coll,
                       const std::string &filter) override {
    if (coll == "push_subscriptions") deleted_filters.push_back(filter);
    return true;
  }
  std::string get_documents(const std::string &coll, const std::string &,
                            const std::string &) override {
    return (coll == "push_subscriptions") ? subs_json : std::string{};
  }
  std::string get_documents(const std::string &coll,
                            const std::string &) override {
    return (coll == "push_subscriptions") ? subs_json : std::string{};
  }
  std::string next_awbno(const std::string &) override { return {}; }
  std::string store_file(const std::string &, const std::string &,
                         const std::vector<std::uint8_t> &) override { return {}; }
  std::vector<std::uint8_t> fetch_file(const std::string &) override { return {}; }
  std::vector<std::uint8_t> fetch_file_by_id(const std::string &) override { return {}; }
  bool delete_file(const std::string &) override { return false; }

private:
  std::string m_db = "pbx";
};

// Decode a JWT (compact JWS) without verifying the signature.
json decode_jwt_claims(const std::string &jwt) {
    const auto dot1 = jwt.find('.');
    const auto dot2 = jwt.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos)
        throw std::runtime_error("decode_jwt_claims: not 3 parts");
    const std::string claims_b64 = jwt.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string claims_raw = push_crypto::b64url_decode(claims_b64);
    return json::parse(claims_raw);
}

PushSender::Config default_config(const std::string &priv_pem) {
    PushSender::Config c;
    c.vapid_private_pem  = priv_pem;
    c.vapid_subject      = "mailto:ops@example.com";
    c.jwt_exp_seconds    = 12 * 60 * 60;
    c.max_retries        = 3;
    c.initial_backoff_ms = 0;   // tests don't sleep
    return c;
}

} // namespace

// ── VAPID JWT ─────────────────────────────────────────────────────────────────

TEST(PushSender, VapidJwt_HasCorrectAudience)
{
    auto kp = push_crypto::p256_generate();
    FakeHttpClient http;
    TestDb db;
    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(kp.private_pem), http, db, clock);

    const std::string jwt = ps.sign_vapid_jwt("https://push.example", clock.t);
    json claims = decode_jwt_claims(jwt);

    EXPECT_EQ("https://push.example", claims["aud"].get<std::string>());
    EXPECT_EQ("mailto:ops@example.com", claims["sub"].get<std::string>());
}

TEST(PushSender, VapidJwt_ExpiresIn12hMax)
{
    auto kp = push_crypto::p256_generate();
    FakeHttpClient http;
    TestDb db;
    const std::int64_t now = 1'700'000'000;
    FixedClock clock(now);
    PushSender ps(default_config(kp.private_pem), http, db, clock);

    const std::string jwt = ps.sign_vapid_jwt("https://push.example", now);
    json claims = decode_jwt_claims(jwt);
    const std::int64_t exp = claims["exp"].get<std::int64_t>();

    // RFC 8292 caps exp at 24 h; our config uses 12 h.
    EXPECT_LE(exp - now, 12 * 60 * 60);
    EXPECT_GT(exp - now, 0);
}

TEST(PushSender, BuildVapidAuth_IncludesPublicKeyAndToken)
{
    auto kp = push_crypto::p256_generate();
    FakeHttpClient http;
    TestDb db;
    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(kp.private_pem), http, db, clock);

    const std::string auth = ps.build_vapid_auth("https://push.example", clock.t);
    EXPECT_NE(std::string::npos, auth.find("vapid t="));
    EXPECT_NE(std::string::npos, auth.find(", k=" + ps.vapid_public_b64url()));
}

// ── Web Push payload encryption (RFC 8291 round-trip) ────────────────────────

TEST(PushSender, EncryptsPayloadAes128Gcm)
{
    // "Browser" side keypair + auth secret.
    auto sub_kp = push_crypto::p256_generate();
    const std::string auth_raw = push_crypto::rand_bytes(16);
    const std::string p256dh_b64 = push_crypto::b64url_encode(sub_kp.public_uncompressed);
    const std::string auth_b64   = push_crypto::b64url_encode(auth_raw);

    // Server side.
    auto srv_kp = push_crypto::p256_generate();
    FakeHttpClient http;
    TestDb db;
    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(srv_kp.private_pem), http, db, clock);

    const std::string plaintext =
        R"({"event":"incoming_call","from":"A-101","callId":"abc123"})";
    const std::string record = ps.encrypt_payload(plaintext, p256dh_b64, auth_b64);

    // Wire-format shape: at least 21 bytes of framing + a non-empty ciphertext.
    ASSERT_GT(record.size(), 21u);
    EXPECT_EQ(65, static_cast<unsigned char>(record[20]))
        << "idlen byte should hold 65 (uncompressed P-256 pubkey size)";

    const std::string recovered =
        push_crypto::decrypt_payload_for_testing(sub_kp.private_pem, auth_raw, record);
    EXPECT_EQ(plaintext, recovered);
}

// ── notify() — retry on 503 ──────────────────────────────────────────────────

namespace {

std::string make_subs_array(const std::string &p256dh_b64,
                            const std::string &auth_b64) {
    json arr = json::array();
    json sub;
    sub["_id"]          = "sub1";
    sub["subscriberId"] = "u1";
    sub["endpoint"]     = "https://push.example/abc";
    sub["p256dh"]       = p256dh_b64;
    sub["auth"]         = auth_b64;
    arr.push_back(sub);
    return arr.dump();
}

} // namespace

TEST(PushSender, RetriesOn503)
{
    auto sub_kp = push_crypto::p256_generate();
    const std::string p256dh_b64 = push_crypto::b64url_encode(sub_kp.public_uncompressed);
    const std::string auth_b64   = push_crypto::b64url_encode(push_crypto::rand_bytes(16));

    auto srv_kp = push_crypto::p256_generate();
    FakeHttpClient http;
    http.scripted = {
        {503, "busy"},
        {503, "still busy"},
        {201, "delivered"},  // succeeds on attempt 3 of 4
    };

    TestDb db;
    db.subs_json = make_subs_array(p256dh_b64, auth_b64);

    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(srv_kp.private_pem), http, db, clock);

    const int delivered = ps.notify("u1", "hello");
    EXPECT_EQ(1, delivered);
    EXPECT_EQ(3u, http.calls)
        << "must have retried twice on 503 before succeeding";
}

TEST(PushSender, GivesUpAfterMaxRetries)
{
    auto sub_kp = push_crypto::p256_generate();
    const std::string p256dh_b64 = push_crypto::b64url_encode(sub_kp.public_uncompressed);
    const std::string auth_b64   = push_crypto::b64url_encode(push_crypto::rand_bytes(16));

    auto srv_kp = push_crypto::p256_generate();
    FakeHttpClient http;
    http.scripted = {
        {503, ""}, {503, ""}, {503, ""}, {503, ""},
    };

    TestDb db;
    db.subs_json = make_subs_array(p256dh_b64, auth_b64);

    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(srv_kp.private_pem), http, db, clock);

    const int delivered = ps.notify("u1", "hello");
    EXPECT_EQ(0, delivered);
    // Initial + 3 retries = 4 attempts, then give up.
    EXPECT_EQ(4u, http.calls);
}

// ── notify() — 410 Gone drops the subscription ───────────────────────────────

TEST(PushSender, DropsSubscriptionOn410Gone)
{
    auto sub_kp = push_crypto::p256_generate();
    const std::string p256dh_b64 = push_crypto::b64url_encode(sub_kp.public_uncompressed);
    const std::string auth_b64   = push_crypto::b64url_encode(push_crypto::rand_bytes(16));

    auto srv_kp = push_crypto::p256_generate();
    FakeHttpClient http;
    http.scripted = { {410, "gone"} };

    TestDb db;
    db.subs_json = make_subs_array(p256dh_b64, auth_b64);

    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(srv_kp.private_pem), http, db, clock);

    const int delivered = ps.notify("u1", "hello");
    EXPECT_EQ(0, delivered);
    EXPECT_EQ(1u, http.calls) << "410 should not be retried";
    ASSERT_EQ(1u, db.deleted_filters.size());
    EXPECT_NE(std::string::npos, db.deleted_filters[0].find(R"("_id":"sub1")"));
}

TEST(PushSender, NoSubscriptions_NotifyReturnsZero)
{
    auto srv_kp = push_crypto::p256_generate();
    FakeHttpClient http;
    TestDb db;  // subs_json empty
    FixedClock clock(1'700'000'000);
    PushSender ps(default_config(srv_kp.private_pem), http, db, clock);

    EXPECT_EQ(0, ps.notify("nobody", "hello"));
    EXPECT_EQ(0u, http.calls);
}

#include "sip_frame.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace SipFrame;

namespace {

// Tiny helper: encode then decode a frame and assert round-trip.
void expect_round_trip(Op op, std::uint32_t sid, const std::string &payload)
{
    const std::string bytes = encode(op, sid, payload);
    EXPECT_EQ(kHeaderSize + payload.size(), bytes.size());

    const auto r = decode(bytes);
    ASSERT_EQ(Status::Ok, r.status);
    EXPECT_EQ(op,         r.frame.op);
    EXPECT_EQ(sid,        r.frame.stream_id);
    EXPECT_EQ(payload,    r.frame.payload);
    EXPECT_EQ(bytes.size(), r.consumed);
}

} // namespace

// ── Round-trip per op ─────────────────────────────────────────────────────────

TEST(SipFrame, SerializeRoundTrip_Open)
{
    expect_round_trip(Op::OPEN, 42,
        R"({"societyId":"S1","sipUsername":"u_alice","clientUA":"Chrome/120"})");
}

TEST(SipFrame, SerializeRoundTrip_Data)
{
    // Binary-safe: include CRLF and a NUL byte mid-payload.
    std::string payload =
        "INVITE sip:b@s SIP/2.0\r\n"
        "Content-Length: 5\r\n\r\n"
        "ab\0de";
    payload.resize(payload.size()); // ensure NUL not truncated
    payload = std::string("INVITE sip:b@s SIP/2.0\r\nContent-Length: 5\r\n\r\nab\0de", 50);
    expect_round_trip(Op::DATA, 7, payload);
}

TEST(SipFrame, SerializeRoundTrip_PingPong)
{
    expect_round_trip(Op::PING, 0, "");
    expect_round_trip(Op::PONG, 0, "");
}

TEST(SipFrame, SerializeRoundTrip_SubscriberRevoked)
{
    // cloud → agent revocation: stream-id unused (0), JSON payload.
    expect_round_trip(Op::SUBSCRIBER_REVOKED, 0,
                      R"({"societyId":"soc1","sipUsername":"u_abc123"})");
}

TEST(SipFrame, SerializeRoundTrip_RegisterState)
{
    // agent → cloud presence update: stream-id unused (0), JSON payload.
    expect_round_trip(Op::REGISTER_STATE, 0,
                      R"({"societyId":"soc1","sipUsername":"u_abc123","online":true})");
}

// ── Error paths ───────────────────────────────────────────────────────────────

TEST(SipFrame, RejectsBadVersion)
{
    std::string buf = encode(Op::DATA, 1, "x");
    buf[0] = static_cast<char>(0x99);  // bogus version

    const auto r = decode(buf);
    EXPECT_EQ(Status::Invalid, r.status);
    EXPECT_EQ(0u, r.consumed);
}

TEST(SipFrame, RejectsBadOp)
{
    std::string buf = encode(Op::DATA, 1, "x");
    buf[1] = static_cast<char>(0xFE);  // unknown op

    const auto r = decode(buf);
    EXPECT_EQ(Status::Invalid, r.status);
}

TEST(SipFrame, RejectsTruncatedHeader)
{
    const std::string bytes = encode(Op::DATA, 1, "payload");
    // Cut the header in half.
    const auto r = decode(bytes.substr(0, kHeaderSize - 3));
    EXPECT_EQ(Status::NeedMore, r.status);
    EXPECT_EQ(0u, r.consumed);
}

TEST(SipFrame, RejectsTruncatedPayload)
{
    const std::string bytes = encode(Op::DATA, 1, "hello-world");  // 11 bytes
    // Header arrived, payload short.
    const auto r = decode(bytes.substr(0, kHeaderSize + 4));
    EXPECT_EQ(Status::NeedMore, r.status);
    EXPECT_EQ(0u, r.consumed);
}

TEST(SipFrame, HandlesPayloadAtBufferBoundary)
{
    // Two frames concatenated; decode must consume only the first one.
    const std::string a = encode(Op::DATA, 1, "AAA");
    const std::string b = encode(Op::DATA, 2, "BBBB");
    const std::string concat = a + b;

    const auto r1 = decode(concat);
    ASSERT_EQ(Status::Ok, r1.status);
    EXPECT_EQ(a.size(), r1.consumed);
    EXPECT_EQ(1u, r1.frame.stream_id);
    EXPECT_EQ("AAA", r1.frame.payload);

    // Second decode picks up after the first.
    const auto r2 = decode(concat.substr(r1.consumed));
    ASSERT_EQ(Status::Ok, r2.status);
    EXPECT_EQ(2u, r2.frame.stream_id);
    EXPECT_EQ("BBBB", r2.frame.payload);
}

TEST(SipFrame, MaxPayloadGuard)
{
    // Hand-craft a header that claims payload-len > 1 MiB.
    std::string buf(kHeaderSize, '\0');
    buf[0] = static_cast<char>(kVersion);
    buf[1] = static_cast<char>(Op::DATA);
    buf[2] = 0; buf[3] = 0; buf[4] = 0; buf[5] = 1;  // stream-id = 1
    // payload-len = kMaxPayload + 1, big-endian
    const std::uint32_t over = static_cast<std::uint32_t>(kMaxPayload) + 1;
    buf[6] = static_cast<char>((over >> 24) & 0xFF);
    buf[7] = static_cast<char>((over >> 16) & 0xFF);
    buf[8] = static_cast<char>((over >> 8)  & 0xFF);
    buf[9] = static_cast<char>(over & 0xFF);

    const auto r = decode(buf);
    EXPECT_EQ(Status::Invalid, r.status);
}

// ── Big-endian byte order ─────────────────────────────────────────────────────

TEST(SipFrame, WireFormat_IsBigEndian)
{
    // Encoded stream-id 0x01020304 must appear as bytes 01 02 03 04 in offsets 2..5.
    const std::string bytes = encode(Op::DATA, 0x01020304, "");
    ASSERT_GE(bytes.size(), 6u);
    EXPECT_EQ(static_cast<unsigned char>(bytes[2]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(bytes[3]), 0x02);
    EXPECT_EQ(static_cast<unsigned char>(bytes[4]), 0x03);
    EXPECT_EQ(static_cast<unsigned char>(bytes[5]), 0x04);
}

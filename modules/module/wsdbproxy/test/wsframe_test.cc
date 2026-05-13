#include "wsframe_test.hpp"

// ── encode ────────────────────────────────────────────────────────────────────

TEST(WsFrame, Encode_SmallPayload_Len7Bit)
{
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto frame = wsframe::encode(payload);

    EXPECT_EQ(frame[0], 0x82);          // FIN + binary opcode
    EXPECT_EQ(frame[1], 10);            // MASK=0, len=10
    ASSERT_EQ(frame.size(), 2 + 10u);
    EXPECT_EQ(std::vector<uint8_t>(frame.begin() + 2, frame.end()), payload);
}

TEST(WsFrame, Encode_126ByteBoundary_Uses16BitLen)
{
    std::vector<uint8_t> payload(126, 0xAA);
    auto frame = wsframe::encode(payload);

    EXPECT_EQ(frame[0], 0x82);
    EXPECT_EQ(frame[1], 126);           // extended length marker
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[3], 126);           // uint16 BE = 126
    EXPECT_EQ(frame.size(), 4 + 126u);
}

TEST(WsFrame, Encode_65536BytePayload_Uses64BitLen)
{
    std::vector<uint8_t> payload(65536, 0xBB);
    auto frame = wsframe::encode(payload);

    EXPECT_EQ(frame[1], 127);           // 8-byte extended length marker
    // bytes 2..9: uint64 BE = 65536 = 0x0000000000010000
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[9], 0x00);
    uint64_t len = 0;
    for (int i = 2; i < 10; ++i) len = (len << 8) | frame[i];
    EXPECT_EQ(len, 65536u);
    EXPECT_EQ(frame.size(), 10 + 65536u);
}

TEST(WsFrame, Encode_ClientMasked_MaskBitSet)
{
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
    // Use a known mask key for reproducibility
    auto frame = wsframe::encode(payload, 0x02, true, 0x12345678u);

    EXPECT_EQ(frame[1] & 0x80, 0x80);   // MASK bit set
    // Header: 2 bytes + 4-byte mask key = 6 bytes total for small payload
    ASSERT_EQ(frame.size(), 2 + 4 + 5u);

    // Extract mask key from frame
    uint8_t mk[4] = {frame[2], frame[3], frame[4], frame[5]};
    // Unmask payload
    std::vector<uint8_t> unmasked(frame.begin() + 6, frame.end());
    for (size_t i = 0; i < unmasked.size(); ++i)
        unmasked[i] ^= mk[i % 4];
    EXPECT_EQ(unmasked, payload);
}

TEST(WsFrame, Encode_PingFrame_CorrectOpcode)
{
    auto frame = wsframe::ping_frame();
    EXPECT_EQ(frame[0], 0x89);  // FIN + ping(0x9)
    EXPECT_EQ(frame[1], 0x00);  // no payload, no mask
}

// ── decode ────────────────────────────────────────────────────────────────────

TEST(WsFrame, Decode_UnmaskedSmallFrame_ExtractsPayload)
{
    std::vector<uint8_t> payload = {10, 20, 30, 40};
    auto frame = wsframe::encode(payload);
    auto result = wsframe::decode(frame);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode,  0x02);
    EXPECT_EQ(result->payload, payload);
    EXPECT_TRUE(frame.empty());  // consumed
}

TEST(WsFrame, Decode_MaskedClientFrame_UnmasksCorrectly)
{
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    auto frame = wsframe::encode(payload, 0x02, true, 0xAABBCCDDu);

    auto result = wsframe::decode(frame);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, payload);
}

TEST(WsFrame, Decode_TwoByteLen_DecodesCorrectly)
{
    std::vector<uint8_t> payload(200, 0x42);
    auto frame = wsframe::encode(payload);
    auto result = wsframe::decode(frame);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, payload);
}

TEST(WsFrame, Decode_IncompleteHeader_ReturnsFalse)
{
    std::vector<uint8_t> buf = {0x82};  // only 1 byte
    auto result = wsframe::decode(buf);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(buf.size(), 1u);  // buffer untouched
}

TEST(WsFrame, Decode_IncompletePayload_ReturnsFalse)
{
    // Header says 100 bytes, but only 50 available
    std::vector<uint8_t> buf;
    buf.push_back(0x82);   // FIN + binary
    buf.push_back(100);    // payload len = 100
    for (int i = 0; i < 50; ++i) buf.push_back(0xCC);

    std::size_t original_size = buf.size();
    auto result = wsframe::decode(buf);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(buf.size(), original_size);  // buffer untouched
}

TEST(WsFrame, Decode_MultipleFramesInBuffer_ConsumesFirstOnly)
{
    std::vector<uint8_t> p1 = {1, 2, 3};
    std::vector<uint8_t> p2 = {4, 5, 6};
    auto f1 = wsframe::encode(p1);
    auto f2 = wsframe::encode(p2);

    std::vector<uint8_t> buf;
    buf.insert(buf.end(), f1.begin(), f1.end());
    buf.insert(buf.end(), f2.begin(), f2.end());

    auto r1 = wsframe::decode(buf);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->payload, p1);
    // buf should now contain only f2
    EXPECT_EQ(buf, f2);

    auto r2 = wsframe::decode(buf);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->payload, p2);
    EXPECT_TRUE(buf.empty());
}

// ── accept_key ────────────────────────────────────────────────────────────────

TEST(WsFrame, AcceptKey_RFC6455Example)
{
    // Known pair from RFC 6455 §1.3
    std::string key      = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    EXPECT_EQ(wsframe::accept_key(key), expected);
}

#include "mongodbc.hpp"

#include <gtest/gtest.h>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// hash_password
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PasswordHashTest, Hash_ProducesModularCryptFormat)
{
    std::string hash = MongodbClient::hash_password("mypassword");
    EXPECT_NE(std::string::npos, hash.find("$pbkdf2-sha256$i="));
    // Must have four $-delimited sections: empty, algorithm, params+salt, hash
    std::size_t first  = hash.find('$');
    std::size_t second = hash.find('$', first + 1);
    std::size_t third  = hash.find('$', second + 1);
    std::size_t fourth = hash.find('$', third + 1);
    EXPECT_NE(std::string::npos, second);
    EXPECT_NE(std::string::npos, third);
    EXPECT_NE(std::string::npos, fourth);
    // No fifth section
    EXPECT_EQ(std::string::npos, hash.find('$', fourth + 1));
}

TEST(PasswordHashTest, Hash_ProducesDifferentSaltsPerCall)
{
    std::string h1 = MongodbClient::hash_password("same_password");
    std::string h2 = MongodbClient::hash_password("same_password");
    EXPECT_NE(h1, h2);
}

TEST(PasswordHashTest, Verify_CorrectPassword_ReturnsTrue)
{
    std::string hash = MongodbClient::hash_password("secret123");
    EXPECT_TRUE(MongodbClient::verify_password("secret123", hash));
}

TEST(PasswordHashTest, Verify_WrongPassword_ReturnsFalse)
{
    std::string hash = MongodbClient::hash_password("secret123");
    EXPECT_FALSE(MongodbClient::verify_password("wrongpassword", hash));
}

TEST(PasswordHashTest, Verify_WrongCase_ReturnsFalse)
{
    std::string hash = MongodbClient::hash_password("Secret123");
    EXPECT_FALSE(MongodbClient::verify_password("secret123", hash));
}

TEST(PasswordHashTest, Hash_EmptyPassword_ProducesValidHash)
{
    std::string hash = MongodbClient::hash_password("");
    EXPECT_NE(std::string::npos, hash.find("$pbkdf2-sha256$i="));
}

TEST(PasswordHashTest, Verify_EmptyPassword_Works)
{
    std::string hash = MongodbClient::hash_password("");
    EXPECT_TRUE(MongodbClient::verify_password("", hash));
    EXPECT_FALSE(MongodbClient::verify_password("x", hash));
}

TEST(PasswordHashTest, Hash_LongPassword_ProducesValidHash)
{
    std::string long_pw(10240, 'x');  // 10 KB password
    std::string hash = MongodbClient::hash_password(long_pw);
    EXPECT_NE(std::string::npos, hash.find("$pbkdf2-sha256$i="));
    EXPECT_TRUE(MongodbClient::verify_password(long_pw, hash));
}

TEST(PasswordHashTest, Verify_EmptyHash_ReturnsFalse)
{
    EXPECT_FALSE(MongodbClient::verify_password("anything", ""));
}

TEST(PasswordHashTest, Verify_MalformedHash_ReturnsFalse)
{
    EXPECT_FALSE(MongodbClient::verify_password("anything", "not-a-valid-hash"));
}

TEST(PasswordHashTest, Verify_WrongAlgorithmInHash_ReturnsFalse)
{
    EXPECT_FALSE(MongodbClient::verify_password("pwd", "$unknown-algo$i=1$salt$hash"));
}

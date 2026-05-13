#ifndef __emailservice_test_cc__
#define __emailservice_test_cc__

#include "emailservice_test.hpp"

void EmailServiceTest::SetUp()
{
    /* Mongo DB interface */
    std::string uri("mongodb://127.0.0.1:27017");
    std::string _dbName("bayt");
    std::uint32_t _pool = 50;

    ACE_UNUSED_ARG(_pool);
    ACE_UNUSED_ARG(_dbName);

    mMongodbc = std::make_unique<MongodbClient>(uri);
    mUser = std::make_unique<SMTP::User>();

}

void EmailServiceTest::TearDown()
{
    mMongodbc.release();
    mUser.release();
}

void EmailServiceTest::TestBody()
{

}

EmailServiceTest::EmailServiceTest(std::string in)
{
    // Initialize members before use — SetUp() is only called by the gtest
    // framework, not when constructing the object directly in a TEST body.
    mMongodbc = std::make_unique<MongodbClient>();
    mUser     = std::make_unique<SMTP::User>();

    // {"subject": "", "to": [user-id@domain.com, user-id1@domain.com], "body": ""}
        /* Check for Query string */

    std::string json_body = in;
    JsonStrVec  out_vec;
    JsonDocList out_list;
    std::string subj;
    std::string body;

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l email request:%s\n"), json_body.c_str()));
    if (auto v = mMongodbc->from_json(json_body, "to");      auto *p = std::get_if<JsonStrVec>(&v))  out_vec  = std::move(*p);
    if (auto v = mMongodbc->from_json(json_body, "subject"); auto *p = std::get_if<std::string>(&v)) subj     = *p;
    if (auto v = mMongodbc->from_json(json_body, "body");    auto *p = std::get_if<std::string>(&v)) body     = *p;
    if (auto v = mMongodbc->from_json(json_body, "files");   auto *p = std::get_if<JsonDocList>(&v)) out_list = std::move(*p);
    for(const auto& elm: out_vec) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l email to list:%s\n"), elm.c_str()));
    }

    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l email subject:%s\n"), subj.c_str()));
    ACE_DEBUG((LM_DEBUG, ACE_TEXT("%D [worker:%t] %M %N:%l email body:%s\n"), body.c_str()));

    SMTP::Account::instance().to_email(out_vec);
    SMTP::Account::instance().email_subject(subj);
    SMTP::Account::instance().email_body(body);

    if(!out_list.empty()) {
        /* e-mail with attachment */
        SMTP::Account::instance().attachment(out_list);
    }
}

// Demonstrate some basic assertions.
TEST(Test1, BasicAssertions) {
  std::stringstream ss("");
  ss << "{\"subject\": \"Test email client\", "
     << "\"to\": [\"naushad.dln@gmail.com\", \"hnm.royal@gmail.com\", \"dream.avenue.gp@gmail.com\"],"
     << "\"body\": \"abcd\","
     << "\"files\": [{\"file-name\": \"/home/mnahmed/PWI_264_Jul.pdf\", \"file-content\":\"testing\"}]"
     << "}";
  std::stringstream response("");
  response << "250-smtp.gmail.com at your service, [45.252.71.217]\r\n"
           << "250-SIZE 35882577\r\n"
           << "250-8BITMIME\r\n"
           << "250-STARTTLS\r\n"
           << "250-ENHANCEDSTATUSCODES\r\n"
           << "250-PIPELINING\r\n"
           << "250-CHUNKING\r\n"
           << "250 SMTPUTF8\r\n";
   
  EmailServiceTest emailTest(ss.str());
  emailTest.mUser->fsm().set_state(SMTP::GREETING());
  emailTest.mUser->rx(response.str());
  // Expect two strings not to be equal.
  EXPECT_STRNE("hello", "world");
  // Expect equality.
  EXPECT_EQ(7 * 6, 42);
}


#endif /*__emailservice_test_cc__*/
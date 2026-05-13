#ifndef __emailservice_test_hpp__
#define __emailservice_test_hpp__

#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <fstream>

#include "emailservice.hpp"
#include "mongodbc.hpp"
#include "webservice.hpp"
#include "http_parser.hpp"

class EmailServiceTest : public ::testing::Test
{
    public:
        EmailServiceTest(std::string in);
        ~EmailServiceTest() = default;
     
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        std::unique_ptr<MongodbClient> mMongodbc; 
        std::unique_ptr<SMTP::User> mUser;

};

#endif /*__emailservice_test_hpp__*/
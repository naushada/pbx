cmake_minimum_required(VERSION 3.16.3)
project(offtarget)
find_package(GTest REQUIRED)

include_directories(../modules/module/http/inc)
include_directories(../modules/module/http/test)
include_directories(../modules/module/sip/inc)
include_directories(../modules/module/sip/test)
include_directories(../modules/module/pbx/inc)
include_directories(../modules/module/pbx/test)
include_directories(/usr/local/ACE_TAO-7.0.0/include)

link_directories(/usr/local/ACE_TAO-7.0.0/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -Wall -Wextra")

file(GLOB SOURCES "*.cc"
    "../modules/module/http/src/message_parser.cpp"
    "../modules/module/http/src/http_parser.cpp"
    "../modules/module/http/test/httpparser_test.cc"
    "../modules/module/http/test/message_parser_test.cc"
    "../modules/module/sip/src/sip_parser.cpp"
    "../modules/module/sip/test/sip_parser_test.cc"
    "../modules/module/pbx/src/sip_frame.cpp"
    "../modules/module/pbx/test/sip_frame_test.cc"
)

enable_testing()

add_executable(offtarget ${SOURCES})
target_link_libraries(offtarget gtest_main)
target_link_libraries(offtarget pthread)
target_link_libraries(offtarget ACE)
target_link_libraries(offtarget gtest)
target_link_libraries(offtarget z)

add_test(offtarget_gtests offtarget)
gtest_discover_tests(offtarget)

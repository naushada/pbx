cmake_minimum_required(VERSION 3.16.3)
project(offtarget)
find_package(GTest REQUIRED)

# mongo-cxx-driver headers (for mongodbc / wsdbproxy / webservice)
include_directories(/usr/local/include/mongocxx/v_noabi)
include_directories(/usr/local/include/bsoncxx/v_noabi)

# Our modules
include_directories(../modules/module/http/inc)
include_directories(../modules/module/http/test)
include_directories(../modules/module/sip/inc)
include_directories(../modules/module/sip/test)
include_directories(../modules/module/pbx/inc)
include_directories(../modules/module/pbx/test)
include_directories(../modules/module/webservice/inc)
include_directories(../modules/module/webservice/test)
include_directories(../modules/module/mongodb/inc)
include_directories(../modules/module/wsdbproxy/inc)
include_directories(../modules/module/wsdbproxy/test)
include_directories(../modules/module/security/inc)
include_directories(../modules/module/email/inc)
include_directories(../modules/module/email/test)
include_directories(../modules/module/thirdparty)

include_directories(/usr/local/ACE_TAO-7.0.0/include)
link_directories(/usr/local/ACE_TAO-7.0.0/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -Wall -Wextra")

file(GLOB SOURCES "*.cc"
    # ── http (Layer 0) ────────────────────────────────────────────────
    "../modules/module/http/src/message_parser.cpp"
    "../modules/module/http/src/http_parser.cpp"
    "../modules/module/http/test/httpparser_test.cc"
    "../modules/module/http/test/message_parser_test.cc"
    # ── sip (Layer 0) ─────────────────────────────────────────────────
    "../modules/module/sip/src/sip_parser.cpp"
    "../modules/module/sip/test/sip_parser_test.cc"
    # ── pbx (Layer 0 + Layer 1 first slice) ───────────────────────────
    "../modules/module/pbx/src/sip_frame.cpp"
    "../modules/module/pbx/test/sip_frame_test.cc"
    "../modules/module/pbx/src/sip_bridge.cpp"
    "../modules/module/pbx/test/sip_bridge_test.cc"
    "../modules/module/pbx/src/microservice_pbx.cpp"
    "../modules/module/pbx/test/microservice_pbx_test.cc"
    # ── webservice (xpmile copy, Layer 1) ─────────────────────────────
    "../modules/module/webservice/src/webservice.cpp"
    "../modules/module/webservice/test/webservice_test.cc"
    # ── mongodb (xpmile copy, Layer 1) ────────────────────────────────
    "../modules/module/mongodb/src/mongodbc.cpp"
    "../modules/module/mongodb/test/mongodbc_test.cc"
    # ── wsdbproxy (xpmile copy, Layer 1) ──────────────────────────────
    "../modules/module/wsdbproxy/src/dbproto.cpp"
    "../modules/module/wsdbproxy/src/wsframe.cpp"
    "../modules/module/wsdbproxy/src/wsdbproxy.cpp"
    "../modules/module/wsdbproxy/test/dbproto_test.cc"
    "../modules/module/wsdbproxy/test/wsframe_test.cc"
    "../modules/module/wsdbproxy/test/wsdbserver_test.cc"
    "../modules/module/wsdbproxy/test/wsproxy_test.cc"
    # ── security (xpmile copy, transitive dependency of wsdbproxy) ────
    "../modules/module/security/src/innertls.cpp"
    "../modules/module/security/test/innertls_test.cc"
    # ── email (xpmile copy, Layer 1) ──────────────────────────────────
    "../modules/module/email/src/emailservice.cpp"
    "../modules/module/email/src/emailservice_fsm.cpp"
    "../modules/module/email/test/emailservice_test.cc"
)

enable_testing()

add_executable(offtarget ${SOURCES})
target_link_libraries(offtarget gtest_main)
target_link_libraries(offtarget pthread)
target_link_libraries(offtarget ACE)
target_link_libraries(offtarget ACE_SSL)
target_link_libraries(offtarget ssl crypto)
target_link_libraries(offtarget mongocxx)
target_link_libraries(offtarget bsoncxx)
target_link_libraries(offtarget gtest)
target_link_libraries(offtarget z)

add_test(offtarget_gtests offtarget)
gtest_discover_tests(offtarget)

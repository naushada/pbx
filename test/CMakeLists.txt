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
include_directories(integration)
include_directories(../pbx-agent/src/main)
include_directories(../pbx-agent/src/test)
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
    "../modules/module/pbx/src/cloud_tunnel_endpoint.cpp"
    "../modules/module/pbx/test/cloud_tunnel_endpoint_test.cc"
    "../modules/module/pbx/src/microservice_pbx.cpp"
    "../modules/module/pbx/test/microservice_pbx_test.cc"
    "../modules/module/pbx/src/push_sender.cpp"
    "../modules/module/pbx/test/push_sender_test.cc"
    "../modules/module/pbx/src/ace_https_client.cpp"
    "../modules/module/pbx/test/ace_https_client_test.cc"
    "../modules/module/pbx/src/browser_stream.cpp"
    "../modules/module/pbx/test/browser_stream_test.cc"
    "../modules/module/pbx/src/agent_stream.cpp"
    "../modules/module/pbx/test/agent_stream_test.cc"
    # ── pbx-agent (Layer 2) ───────────────────────────────────────────
    "../pbx-agent/src/main/sip_frame_demux.cpp"
    "../pbx-agent/src/test/sip_frame_demux_test.cc"
    "../pbx-agent/src/main/cloud_connector.cpp"
    "../pbx-agent/src/test/cloud_connector_test.cc"
    "../pbx-agent/src/main/ari_client.cpp"
    "../pbx-agent/src/test/ari_client_test.cc"
    "../pbx-agent/src/main/ace_ssl_transport.cpp"
    "../pbx-agent/src/test/ace_ssl_transport_test.cc"
    "../pbx-agent/src/main/ari_ws_client.cpp"
    "../pbx-agent/src/test/ari_ws_client_test.cc"
    "../pbx-agent/src/main/asterisk_ws_factory.cpp"
    "../pbx-agent/src/test/asterisk_ws_factory_test.cc"
    "../pbx-agent/src/main/ari_rest_client.cpp"
    "../pbx-agent/src/test/ari_rest_client_test.cc"
    "../pbx-agent/src/main/call_router.cpp"
    "../pbx-agent/src/test/call_router_test.cc"
    # ── Layer 3 integration ──────────────────────────────────────────
    "integration/tunnel_e2e_test.cc"
    "integration/handoff_ordering_test.cc"
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

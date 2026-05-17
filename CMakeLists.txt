cmake_minimum_required(VERSION 3.16.3)
project(onprem-pbx)

option(BUILD_TESTS    "Build the offtarget GTest binary" ON)
option(BUILD_BINARIES "Build the production pbx-agent and pbx-cloud binaries" ON)

include_directories(/usr/local/include/mongocxx/v_noabi)
include_directories(/usr/local/include/bsoncxx/v_noabi)
include_directories(modules/module/http/inc)
include_directories(modules/module/sip/inc)
include_directories(modules/module/pbx/inc)
include_directories(modules/module/webservice/inc)
include_directories(modules/module/mongodb/inc)
include_directories(modules/module/wsdbproxy/inc)
include_directories(modules/module/wsdbagent/inc)
include_directories(modules/module/security/inc)
include_directories(modules/module/email/inc)
include_directories(modules/module/thirdparty)
include_directories(pbx-agent/src/main)
include_directories(/usr/local/ACE_TAO-7.0.0/include)

link_directories(/usr/local/ACE_TAO-7.0.0/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -Wall -Wextra")

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Production binaries.
#
# pbx-agent : on-prem daemon (Layer 2 + Layer 3 ACE bindings + Layer 4 main).
# pbx-cloud : Heroku-side server, structurally identical to xpmile's
#             uniservice but with our SipBridge/CloudTunnelEndpoint
#             bootstrap patched into webservice_main.cpp.
# ─────────────────────────────────────────────────────────────────────────────

if(BUILD_BINARIES)
  # ── pbx-agent ──────────────────────────────────────────────────────────
  file(GLOB PBX_AGENT_MAIN_SRC
       "pbx-agent/src/main/*.cpp")
  set(PBX_AGENT_DEPS
      modules/module/http/src/message_parser.cpp
      modules/module/http/src/http_parser.cpp
      modules/module/sip/src/sip_parser.cpp
      modules/module/pbx/src/sip_frame.cpp
      modules/module/pbx/src/sip_bridge.cpp
      modules/module/wsdbproxy/src/wsframe.cpp
      modules/module/mongodb/src/mongodbc.cpp
      # InnerTLS over the /agent WS tunnel — same mTLS trust boundary
      # `/ws/db` already gets. See DESIGN.md §3.2 + ws_inner_tls_bridge.hpp.
      modules/module/security/src/innertls.cpp
      modules/module/security/src/ws_inner_tls_bridge.cpp)
  # AsteriskWsFactory is part of pbx-agent/src/main, picked up via the
  # glob above. No additional source list needed.
  add_executable(pbx-agent ${PBX_AGENT_MAIN_SRC} ${PBX_AGENT_DEPS})
  target_link_libraries(pbx-agent pthread)
  target_link_libraries(pbx-agent ACE)
  target_link_libraries(pbx-agent ACE_SSL)
  target_link_libraries(pbx-agent ssl crypto)
  target_link_libraries(pbx-agent mongocxx bsoncxx)
  target_link_libraries(pbx-agent z)

  # ── pbx-cloud ──────────────────────────────────────────────────────────
  # Mirrors xpmile's uniservice target but with our patched webservice_main.
  file(GLOB PBX_CLOUD_SRC
       "modules/module/webservice/src/*.cpp"
       "modules/module/http/src/*.cpp"
       "modules/module/email/src/*.cpp"
       "modules/module/wsdbproxy/src/*.cpp"
       "modules/module/security/src/*.cpp"
       "modules/module/mongodb/src/*.cpp"
       "modules/module/pbx/src/sip_frame.cpp"
       "modules/module/pbx/src/sip_bridge.cpp"
       "modules/module/pbx/src/cloud_tunnel_endpoint.cpp"
       "modules/module/pbx/src/cloud_tunnel_tick_driver.cpp"
       "modules/module/pbx/src/browser_stream.cpp"
       "modules/module/pbx/src/agent_stream.cpp"
       "modules/module/pbx/src/microservice_pbx.cpp"
       "modules/module/pbx/src/push_sender.cpp"
       "modules/module/pbx/src/ace_https_client.cpp")
  add_executable(pbx-cloud ${PBX_CLOUD_SRC})
  target_link_libraries(pbx-cloud pthread)
  target_link_libraries(pbx-cloud ACE)
  target_link_libraries(pbx-cloud ACE_SSL)
  target_link_libraries(pbx-cloud ssl crypto)
  target_link_libraries(pbx-cloud mongocxx bsoncxx)
  target_link_libraries(pbx-cloud z)

  # ── wsdbagent ──────────────────────────────────────────────────────────
  # Standalone on-prem binary that dials wss://${CLOUD_HOST}/ws/db with
  # ACE InnerTLS on top of the outer WSS (Heroku-router TLS terminates
  # the outer; InnerTLS gives us real mTLS to the cloud process). Each
  # binary frame carries one BSON-encoded DbRequest / DbResponse.
  # Verbatim copy from xpmile/modules/module/wsdbagent.
  file(GLOB MODULE_WSDBAGENT_SOURCES
       "modules/module/wsdbagent/src/*.cpp")
  add_executable(wsdbagent
      ${MODULE_WSDBAGENT_SOURCES}
      modules/module/wsdbproxy/src/dbproto.cpp
      modules/module/wsdbproxy/src/wsframe.cpp
      modules/module/security/src/innertls.cpp
      modules/module/mongodb/src/mongodbc.cpp)
  target_link_libraries(wsdbagent pthread)
  target_link_libraries(wsdbagent ACE)
  target_link_libraries(wsdbagent ACE_SSL)
  target_link_libraries(wsdbagent ssl crypto)
  target_link_libraries(wsdbagent mongocxx bsoncxx)
endif()

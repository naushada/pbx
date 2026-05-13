cmake_minimum_required(VERSION 3.16.3)
project(onprem-pbx)

option(BUILD_TESTS "Build the offtarget GTest binary" ON)

include_directories(modules/module/http/inc)
include_directories(modules/module/sip/inc)
include_directories(modules/module/pbx/inc)
include_directories(/usr/local/ACE_TAO-7.0.0/include)

link_directories(/usr/local/ACE_TAO-7.0.0/lib)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -Wall -Wextra")

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

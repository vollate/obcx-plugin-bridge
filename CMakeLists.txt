cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(obcx_actor_bridge VERSION 0.1.0 LANGUAGES CXX)
  find_package(obcx-sdk CONFIG REQUIRED)
  include(CTest)
  set(_obcx_bridge_build_tests_default ${BUILD_TESTING})
else()
  set(_obcx_bridge_build_tests_default OFF)
endif()
option(OBCX_BRIDGE_BUILD_TESTS
       "Build the Bridge actor repository-owned test suite"
       ${_obcx_bridge_build_tests_default})
option(OBCX_BRIDGE_CONFORMANCE_TESTS
       "Build installed Message Store/Bridge pipeline and reload smoke tests"
       OFF)
set(OBCX_MESSAGE_STORE_ACTOR_PATH "" CACHE FILEPATH
    "Installed Message Store actor used by Bridge conformance smoke tests")
set(OBCX_BRIDGE_CONFORMANCE_INSTALL_PREFIX "" CACHE PATH
    "Shared installed-SDK prefix used by Bridge conformance smoke tests")
set(OBCX_CORE_SOURCE_DIR "" CACHE PATH
    "OBCX source tree providing non-installed runtime conformance headers")
unset(_obcx_bridge_build_tests_default)

if(OBCX_BRIDGE_CONFORMANCE_TESTS AND NOT OBCX_BRIDGE_BUILD_TESTS)
  message(FATAL_ERROR
          "OBCX_BRIDGE_CONFORMANCE_TESTS requires OBCX_BRIDGE_BUILD_TESTS")
endif()

find_package(tomlplusplus REQUIRED)
include(OBCXActor)

include_directories(include)

add_subdirectory(dependency)

obcx_add_actor(bridge
  SOURCES actor/bridge_actor.cpp
  DEPS bridge_core
  OUTPUT_NAME bridge)
target_include_directories(bridge_actor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(OBCX_BRIDGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

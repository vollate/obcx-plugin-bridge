cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(obcx_actor_bridge VERSION 0.1.0 LANGUAGES CXX)
  find_package(obcx-sdk CONFIG REQUIRED)
  include(CTest)
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

if(OBCX_BUILD_TESTS OR BUILD_TESTING)
  add_subdirectory(tests)
endif()

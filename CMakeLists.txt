cmake_minimum_required(VERSION 3.20)
project(obcx_actor_bridge VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

find_package(obcx-sdk REQUIRED)
find_package(tomlplusplus REQUIRED)

include_directories(include)

add_subdirectory(dependency)

include(OBCXActor)
obcx_add_actor(bridge
  SOURCES actor/bridge_actor.cpp
  DEPS bridge_core
  OUTPUT_NAME bridge)
target_include_directories(bridge_actor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(OBCX_BUILD_TESTS OR BUILD_TESTING)
  add_subdirectory(tests)
endif()

find_package(tomlplusplus REQUIRED)

include_directories(include)

add_subdirectory(dependency)

include(OBCXActor)
obcx_add_actor(bridge
  SOURCES actor/bridge_actor.cpp
  DEPS bridge_core
  OUTPUT_NAME bridge)
target_include_directories(bridge_actor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(qq_to_tg)
add_subdirectory(tg_to_qq)

if(OBCX_BUILD_TESTS OR BUILD_TESTING)
  add_subdirectory(tests)
endif()

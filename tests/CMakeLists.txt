find_package(GTest REQUIRED)

add_executable(bridge_database_schema_test database_schema_test.cpp)
target_include_directories(
  bridge_database_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                      ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(bridge_database_schema_test PRIVATE bridge_core
                                                          GTest::gtest_main)

add_executable(bridge_actor_test bridge_actor_test.cpp
                                 ../actor/bridge_actor.cpp)
target_include_directories(
  bridge_actor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                            ${CMAKE_SOURCE_DIR}/include)
if(IS_ABSOLUTE "${OBCX_CORE_SOURCE_DIR}" AND
   EXISTS "${OBCX_CORE_SOURCE_DIR}/include/core/qq_bot.hpp")
  target_include_directories(bridge_actor_test
                             PRIVATE ${OBCX_CORE_SOURCE_DIR}/include)
endif()
target_link_libraries(bridge_actor_test PRIVATE bridge_core GTest::gtest_main)
target_compile_definitions(bridge_actor_test
                           PRIVATE OBCX_BRIDGE_ACTOR_NO_EXPORT)

add_executable(command_operation_test command_operation_test.cpp)
target_include_directories(
  command_operation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                 ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(command_operation_test PRIVATE bridge_core
                                                     GTest::gtest_main)

add_executable(retry_queue_manager_test retry_queue_manager_test.cpp)
target_include_directories(
  retry_queue_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                   ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(retry_queue_manager_test PRIVATE bridge_core
                                                       GTest::gtest_main)

add_executable(handler_repository_test handler_repository_test.cpp)
target_include_directories(
  handler_repository_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                  ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(handler_repository_test PRIVATE bridge_core
                                                      GTest::gtest_main)
target_compile_definitions(
  handler_repository_test
  PRIVATE OBCX_BRIDGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(received_message_repository_test
               received_message_repository_test.cpp)
target_include_directories(
  received_message_repository_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(received_message_repository_test
                      PRIVATE bridge_core GTest::gtest_main)

add_executable(message_event_adapter_test message_event_adapter_test.cpp)
target_include_directories(
  message_event_adapter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                     ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(message_event_adapter_test PRIVATE bridge_core
                                                         GTest::gtest_main)

add_executable(path_manager_test path_manager_test.cpp)
target_include_directories(
  path_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                            ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(path_manager_test PRIVATE bridge_core GTest::gtest_main)

add_executable(image_url_validator_test image_url_validator_test.cpp)
target_include_directories(
  image_url_validator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                   ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(image_url_validator_test PRIVATE bridge_core
                                                       GTest::gtest_main)

add_executable(photo_normalizer_test photo_normalizer_test.cpp)
target_include_directories(
  photo_normalizer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(photo_normalizer_test PRIVATE bridge_core
                                                    GTest::gtest_main)

add_executable(bridge_db_runtime_test bridge_db_runtime_test.cpp)
target_include_directories(
  bridge_db_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                 ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(bridge_db_runtime_test PRIVATE bridge_core
                                                     GTest::gtest_main)

include(GoogleTest)
set(_bridge_test_labels "unit\\;actor-package\\;bridge")
gtest_discover_tests(bridge_database_schema_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(bridge_actor_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(command_operation_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(retry_queue_manager_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(handler_repository_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(received_message_repository_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(message_event_adapter_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(path_manager_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(image_url_validator_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(photo_normalizer_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")
gtest_discover_tests(bridge_db_runtime_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")

if(OBCX_BRIDGE_CONFORMANCE_TESTS)
  include(GNUInstallDirs)
  if(NOT IS_ABSOLUTE "${OBCX_MESSAGE_STORE_ACTOR_PATH}" OR
     NOT EXISTS "${OBCX_MESSAGE_STORE_ACTOR_PATH}")
    message(FATAL_ERROR
            "Bridge conformance requires an installed Message Store actor path")
  endif()
  if(NOT IS_ABSOLUTE "${OBCX_BRIDGE_CONFORMANCE_INSTALL_PREFIX}")
    message(FATAL_ERROR
            "Bridge conformance requires an absolute shared install prefix")
  endif()
  if(NOT IS_ABSOLUTE "${OBCX_CORE_SOURCE_DIR}" OR
     NOT EXISTS "${OBCX_CORE_SOURCE_DIR}/include/core/orchestrator.hpp")
    message(FATAL_ERROR
            "Bridge runtime conformance requires the coordinated OBCX source tree")
  endif()

  add_executable(standalone_actor_pipeline_smoke
                 standalone_actor_pipeline_smoke.cpp)
  target_include_directories(
    standalone_actor_pipeline_smoke
    PRIVATE ${PROJECT_SOURCE_DIR}/include ${OBCX_CORE_SOURCE_DIR}/include)
  target_link_libraries(standalone_actor_pipeline_smoke
                        PRIVATE obcx::obcx_core)

  add_executable(standalone_actor_reload_smoke
                 standalone_actor_reload_smoke.cpp)
  target_include_directories(standalone_actor_reload_smoke
                             PRIVATE ${OBCX_CORE_SOURCE_DIR}/include)
  target_link_libraries(standalone_actor_reload_smoke
                        PRIVATE obcx::obcx_core)

  if(APPLE)
    set_target_properties(
      standalone_actor_pipeline_smoke standalone_actor_reload_smoke
      PROPERTIES INSTALL_RPATH "@loader_path/../../${CMAKE_INSTALL_LIBDIR}")
  elseif(UNIX)
    set_target_properties(
      standalone_actor_pipeline_smoke standalone_actor_reload_smoke
      PROPERTIES INSTALL_RPATH "\$ORIGIN/../../${CMAKE_INSTALL_LIBDIR}")
  endif()
  install(TARGETS standalone_actor_pipeline_smoke
                  standalone_actor_reload_smoke
          RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/obcx)

  set(_installed_bridge_actor
      "${OBCX_BRIDGE_CONFORMANCE_INSTALL_PREFIX}/lib/obcx/actors/bridge${CMAKE_SHARED_LIBRARY_SUFFIX}")
  add_test(
    NAME BridgeInstalledPipelineSmoke
    COMMAND standalone_actor_pipeline_smoke
            "${OBCX_MESSAGE_STORE_ACTOR_PATH}" "${_installed_bridge_actor}")
  add_test(
    NAME BridgeInstalledReloadSmoke
    COMMAND standalone_actor_reload_smoke
            "${OBCX_MESSAGE_STORE_ACTOR_PATH}" "${_installed_bridge_actor}")
  set_tests_properties(
    BridgeInstalledPipelineSmoke BridgeInstalledReloadSmoke
    PROPERTIES LABELS "integration;conformance;actor-package;bridge"
               TIMEOUT 90)
endif()

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
target_link_libraries(bridge_actor_test PRIVATE bridge_core GTest::gtest_main)
target_compile_definitions(bridge_actor_test
                           PRIVATE OBCX_BRIDGE_ACTOR_NO_EXPORT)

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
gtest_discover_tests(bridge_db_runtime_test DISCOVERY_MODE PRE_TEST
                     PROPERTIES LABELS "${_bridge_test_labels}")

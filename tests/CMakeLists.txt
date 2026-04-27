find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(bridge_tg_to_qq_retry_timeout_test
               tg_to_qq_retry_timeout_test.cpp)
add_executable(bridge_qq_to_tg_retry_timeout_test
               qq_to_tg_retry_timeout_test.cpp)

target_include_directories(
  bridge_tg_to_qq_retry_timeout_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
          ${CMAKE_CURRENT_SOURCE_DIR}/../tg_to_qq
          ${CMAKE_SOURCE_DIR}/include)
target_include_directories(
  bridge_qq_to_tg_retry_timeout_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
          ${CMAKE_CURRENT_SOURCE_DIR}/../qq_to_tg
          ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(
  bridge_tg_to_qq_retry_timeout_test
  PRIVATE tg_to_qq_plugin bridge_core obcx_core GTest::gtest_main)
target_link_libraries(
  bridge_qq_to_tg_retry_timeout_test
  PRIVATE qq_to_tg_plugin bridge_core obcx_core GTest::gtest_main)

gtest_discover_tests(bridge_tg_to_qq_retry_timeout_test DISCOVERY_MODE
                     PRE_TEST)
gtest_discover_tests(bridge_qq_to_tg_retry_timeout_test DISCOVERY_MODE
                     PRE_TEST)

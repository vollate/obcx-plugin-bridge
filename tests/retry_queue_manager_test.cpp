#include "bridge_state_repository.hpp"
#include "core/actor_db.hpp"
#include "core/db_manager.hpp"
#include "retry_queue_manager.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace {

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("obcx_retry_queue_" + name + "_" + std::to_string(stamp) +
          ".sqlite3");
}

auto sqlite_config(const std::filesystem::path &path)
    -> obcx::common::DbInstanceConfig {
  obcx::common::DbInstanceConfig config;
  config.name = "main";
  config.type = "sqlite";
  config.path = path.string();
  return config;
}

auto text_message(const std::string &text) -> obcx::common::Message {
  obcx::common::Message message;
  obcx::common::MessageSegment segment;
  segment.type = "text";
  segment.data["text"] = text;
  message.push_back(std::move(segment));
  return message;
}

} // namespace

TEST(RetryQueueManagerTest, RestoresMessageRetriesFromBridgeStateRepository) {
  const auto db_path = temp_db_path("restore_message_retry");

  obcx::core::DbManager first_db_manager;
  first_db_manager.configure({sqlite_config(db_path)});
  auto first_repository = std::make_shared<bridge::BridgeStateRepository>(
      first_db_manager, "main", "bridge");
  first_repository->initialize_schema();

  boost::asio::io_context first_ioc;
  bridge::RetryQueueManager first_manager(first_ioc, first_repository);
  first_manager.add_message_retry("qq", "telegram", "qq-restore-1",
                                  text_message("hello"), "tg-group", "qq-group",
                                  7, 5, "timeout");
  EXPECT_EQ(first_manager.get_pending_message_retry_count(), 1);

  obcx::core::DbManager second_db_manager;
  second_db_manager.configure({sqlite_config(db_path)});
  auto second_repository = std::make_shared<bridge::BridgeStateRepository>(
      second_db_manager, "main", "bridge");
  second_repository->initialize_schema();

  boost::asio::io_context second_ioc;
  bridge::RetryQueueManager second_manager(second_ioc, second_repository);
  second_manager.restore_persisted_message_retries();

  EXPECT_EQ(second_manager.get_pending_message_retry_count(), 1);

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, RemovesPersistedMessageRetryAfterSuccess) {
  const auto db_path = temp_db_path("remove_after_success");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  storage::MessageRetryInfo retry;
  retry.source_platform = "qq";
  retry.target_platform = "telegram";
  retry.source_message_id = "qq-success-1";
  retry.message_content = R"([{"type":"text","data":{"text":"hello"}}])";
  retry.group_id = "tg-group";
  retry.source_group_id = "qq-group";
  retry.target_topic_id = -1;
  retry.retry_count = 0;
  retry.max_retry_count = 5;
  retry.failure_reason = "timeout";
  retry.retry_type = "message_send";
  retry.next_retry_at = std::chrono::system_clock::now();
  retry.created_at = std::chrono::system_clock::now();
  retry.last_attempt_at = std::chrono::system_clock::now();
  ASSERT_TRUE(repository->add_message_retry(retry));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();

  bool callback_called = false;
  manager.register_message_send_callback(
      "telegram",
      [&callback_called](const bridge::MessageRetryEntry &,
                         const obcx::common::Message &)
          -> boost::asio::awaitable<std::optional<std::string>> {
        callback_called = true;
        co_return std::string{"tg-sent"};
      });

  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, PersistsMappingAfterSuccessfulMessageRetry) {
  const auto db_path = temp_db_path("mapping_after_success");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  storage::MessageRetryInfo retry;
  retry.source_platform = "qq";
  retry.target_platform = "telegram";
  retry.source_message_id = "qq-success-map";
  retry.message_content = R"([{"type":"text","data":{"text":"hello"}}])";
  retry.group_id = "tg-group";
  retry.source_group_id = "qq-group";
  retry.target_topic_id = -1;
  retry.retry_count = 0;
  retry.max_retry_count = 5;
  retry.failure_reason = "timeout";
  retry.retry_type = "message_send";
  retry.next_retry_at = std::chrono::system_clock::now();
  retry.created_at = std::chrono::system_clock::now();
  retry.last_attempt_at = std::chrono::system_clock::now();
  ASSERT_TRUE(repository->add_message_retry(retry));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  manager.register_message_send_callback(
      "telegram",
      [](const bridge::MessageRetryEntry &, const obcx::common::Message &)
          -> boost::asio::awaitable<std::optional<std::string>> {
        co_return std::string{"tg-retry-success"};
      });

  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(
      repository->get_target_message_id("qq", "qq-success-map", "telegram"),
      "tg-retry-success");

  std::filesystem::remove(db_path);
}

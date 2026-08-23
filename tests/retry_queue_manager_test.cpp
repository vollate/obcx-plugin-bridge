#include "bridge_state_repository.hpp"
#include "common/config_loader.hpp"
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

auto ready_retry(std::string source_message_id) -> storage::MessageRetryInfo {
  storage::MessageRetryInfo retry;
  retry.source_installation = "qq";
  retry.source_platform = "qq";
  retry.source_conversation_id = "group:qq-group";
  retry.target_installation = "telegram";
  retry.target_platform = "telegram";
  retry.target_conversation_id = "chat:tg-group";
  retry.source_message_id = std::move(source_message_id);
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
  return retry;
}

auto mapped_target(bridge::BridgeStateRepository &repository,
                   const std::string &source_installation,
                   const std::string &source_conversation,
                   const std::string &source_message_id,
                   const std::string &target_installation,
                   const std::string &target_conversation)
    -> std::optional<std::string> {
  const auto result = repository.resolve_target_mapping(
      {.installation_id = source_installation,
       .platform = "qq",
       .conversation_id = source_conversation,
       .message_id = source_message_id},
      {.installation_id = target_installation,
       .platform = "telegram",
       .conversation_id = target_conversation});
  return result.unique()
             ? std::optional<std::string>{result.mapping->target_message_id}
             : std::nullopt;
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
  first_manager.add_message_retry("qq", "qq", "group:qq-group", "telegram",
                                  "telegram", "chat:tg-group", "qq-restore-1",
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

TEST(RetryQueueManagerTest,
     DuplicateMessageIdentityReplacesInMemoryAndDurableRow) {
  const auto db_path = temp_db_path("deduplicate_message_retry");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(
      ioc, repository,
      bridge::RetryQueuePolicy{.message_retry_base_interval_sec = 7,
                               .media_retry_base_interval_sec = 5,
                               .retry_queue_check_interval_sec = 1,
                               .max_retry_interval_sec = 30});
  const auto before = std::chrono::system_clock::now();
  manager.add_message_retry("qq", "qq", "group:qq-group", "telegram",
                            "telegram", "chat:tg-group", "qq-duplicate-1",
                            text_message("first"), "tg-group", "qq-group", -1,
                            5, "first failure");
  manager.add_message_retry("qq", "qq", "group:qq-group", "telegram",
                            "telegram", "chat:tg-group", "qq-duplicate-1",
                            text_message("second"), "tg-group", "qq-group", 42,
                            6, "second failure");

  EXPECT_EQ(manager.get_pending_message_retry_count(), 1);
  const auto persisted = repository->get_pending_message_retries(
      std::chrono::system_clock::now() + std::chrono::hours{1}, 10);
  ASSERT_EQ(persisted.size(), 1);
  EXPECT_EQ(persisted.front().group_id, "tg-group");
  EXPECT_EQ(persisted.front().target_topic_id, 42);
  EXPECT_EQ(persisted.front().max_retry_count, 6);
  EXPECT_GE(persisted.front().next_retry_at, before + std::chrono::seconds{6});

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, ConfiguredBackoffIsAppliedAndCapped) {
  const auto db_path = temp_db_path("configured_backoff");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  storage::MessageRetryInfo retry;
  retry.source_installation = "qq";
  retry.source_platform = "qq";
  retry.source_conversation_id = "group:qq-group";
  retry.target_installation = "telegram";
  retry.target_platform = "telegram";
  retry.target_conversation_id = "chat:tg-group";
  retry.source_message_id = "qq-backoff-1";
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
  bridge::RetryQueueManager manager(
      ioc, repository,
      bridge::RetryQueuePolicy{.message_retry_base_interval_sec = 3,
                               .media_retry_base_interval_sec = 5,
                               .retry_queue_check_interval_sec = 1,
                               .max_retry_interval_sec = 5});
  manager.restore_persisted_message_retries();
  manager.register_message_send_callback(
      "telegram",
      [](const bridge::MessageRetryEntry &, const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        co_return bridge::MessageSendOutcome::retryable("test failure");
      });
  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  const auto after = std::chrono::system_clock::now();
  const auto persisted = repository->get_pending_message_retries(
      after + std::chrono::hours{1}, 10);
  ASSERT_EQ(persisted.size(), 1);
  EXPECT_EQ(persisted.front().retry_count, 1);
  EXPECT_GE(persisted.front().next_retry_at, after + std::chrono::seconds{4});
  EXPECT_LE(persisted.front().next_retry_at, after + std::chrono::seconds{6});

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, ExhaustedMessageRetryIsRemoved) {
  const auto db_path = temp_db_path("exhausted_message_retry");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  storage::MessageRetryInfo retry;
  retry.source_installation = "telegram";
  retry.source_platform = "telegram";
  retry.source_conversation_id = "chat:tg-group";
  retry.target_installation = "qq";
  retry.target_platform = "qq";
  retry.target_conversation_id = "group:qq-group";
  retry.source_message_id = "tg-exhausted-1";
  retry.message_content = R"([{"type":"text","data":{"text":"hello"}}])";
  retry.group_id = "qq-group";
  retry.source_group_id = "tg-group";
  retry.target_topic_id = -1;
  retry.retry_count = 0;
  retry.max_retry_count = 1;
  retry.failure_reason = "timeout";
  retry.retry_type = "message_send";
  retry.next_retry_at = std::chrono::system_clock::now();
  retry.created_at = std::chrono::system_clock::now();
  retry.last_attempt_at = std::chrono::system_clock::now();
  ASSERT_TRUE(repository->add_message_retry(retry));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(
      ioc, repository,
      bridge::RetryQueuePolicy{.message_retry_base_interval_sec = 1,
                               .media_retry_base_interval_sec = 1,
                               .retry_queue_check_interval_sec = 1,
                               .max_retry_interval_sec = 2});
  manager.restore_persisted_message_retries();
  manager.register_message_send_callback(
      "qq",
      [](const bridge::MessageRetryEntry &, const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        co_return bridge::MessageSendOutcome::retryable("test failure");
      });
  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

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
  retry.source_installation = "qq";
  retry.source_platform = "qq";
  retry.source_conversation_id = "group:qq-group";
  retry.target_installation = "telegram";
  retry.target_platform = "telegram";
  retry.target_conversation_id = "chat:tg-group";
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
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        callback_called = true;
        co_return bridge::MessageSendOutcome::completed("tg-sent");
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
  retry.source_installation = "qq";
  retry.source_platform = "qq";
  retry.source_conversation_id = "group:qq-group";
  retry.target_installation = "telegram";
  retry.target_platform = "telegram";
  retry.target_conversation_id = "chat:tg-group";
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
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        co_return bridge::MessageSendOutcome::completed("tg-retry-success");
      });

  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(mapped_target(*repository, "qq", "group:qq-group", "qq-success-map",
                          "telegram", "chat:tg-group"),
            "tg-retry-success");

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest,
     MappingPersistenceFailureTerminalizesWithoutResend) {
  const auto db_path = temp_db_path("mapping_persistence_failure");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_retry(ready_retry("qq-map-failure")));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  manager.register_message_send_callback(
      "telegram",
      [&db_manager](const bridge::MessageRetryEntry &,
                    const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        db_manager.run_write<void>("main",
                                   [](obcx::core::IDbConnection &connection) {
                                     connection.execute(R"(
                CREATE TRIGGER fail_bridge_mapping_insert
                BEFORE INSERT ON bridge_message_mappings
                BEGIN
                  SELECT RAISE(FAIL, 'injected mapping persistence failure');
                END;
              )");
                                   });
        co_return bridge::MessageSendOutcome::completed("tg-uncommitted");
      });
  manager.start();

  bool pending_before_stop = false;
  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    pending_before_stop = manager.get_pending_message_retry_count() == 1;
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_FALSE(pending_before_stop);
  EXPECT_FALSE(mapped_target(*repository, "qq", "group:qq-group",
                             "qq-map-failure", "telegram", "chat:tg-group")
                   .has_value());
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());
  const auto terminal = db_manager.run_read<bool>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(R"(
          SELECT retry_count, max_retry_count, failure_reason
          FROM bridge_message_retry_queue
          WHERE source_message_id = 'qq-map-failure';
        )");
        return rows.size() == 1 &&
               std::get<std::int64_t>(rows.front().at("retry_count")) ==
                   std::get<std::int64_t>(rows.front().at("max_retry_count")) &&
               std::get<std::string>(rows.front().at("failure_reason")) ==
                   "retry mapping persistence failed";
      });
  EXPECT_TRUE(terminal);

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, CleanupFailureKeepsMappingAndRetryPending) {
  const auto db_path = temp_db_path("cleanup_failure");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_retry(ready_retry("qq-cleanup-failure")));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  manager.register_message_send_callback(
      "telegram",
      [&db_manager](const bridge::MessageRetryEntry &,
                    const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        db_manager.run_write<void>("main",
                                   [](obcx::core::IDbConnection &connection) {
                                     connection.execute(R"(
                CREATE TRIGGER fail_bridge_retry_delete
                BEFORE DELETE ON bridge_message_retry_queue
                BEGIN
                  SELECT RAISE(FAIL, 'injected retry cleanup failure');
                END;
              )");
                                   });
        co_return bridge::MessageSendOutcome::completed(
            "tg-mapped-before-cleanup");
      });
  manager.start();

  bool pending_before_stop = false;
  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    pending_before_stop = manager.get_pending_message_retry_count() == 1;
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_TRUE(pending_before_stop);
  EXPECT_EQ(mapped_target(*repository, "qq", "group:qq-group",
                          "qq-cleanup-failure", "telegram", "chat:tg-group"),
            "tg-mapped-before-cleanup");
  const auto pending = repository->get_pending_message_retries(
      std::chrono::system_clock::now() + std::chrono::hours{1}, 10);
  ASSERT_EQ(pending.size(), 1);
  EXPECT_EQ(pending.front().failure_reason, "retry row cleanup failed");

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, OutcomeUnknownIsTerminalizedWithoutAnotherAttempt) {
  const auto db_path = temp_db_path("outcome_unknown");
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_retry(ready_retry("qq-unknown")));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(
      ioc, repository,
      bridge::RetryQueuePolicy{.message_retry_base_interval_sec = 1,
                               .media_retry_base_interval_sec = 1,
                               .retry_queue_check_interval_sec = 1,
                               .max_retry_interval_sec = 2});
  manager.restore_persisted_message_retries();
  int calls = 0;
  manager.register_message_send_callback(
      "telegram",
      [&calls](const bridge::MessageRetryEntry &, const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        ++calls;
        co_return bridge::MessageSendOutcome::unknown("transport_failure");
      });
  manager.start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{50});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(manager.get_pending_message_retry_count(), 0U);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());
  const auto reason = db_manager.run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(R"(
          SELECT failure_reason FROM bridge_message_retry_queue
          WHERE source_message_id = 'qq-unknown';
        )");
        return rows.empty()
                   ? std::string{}
                   : std::get<std::string>(rows.front().at("failure_reason"));
      });
  EXPECT_EQ(reason, "outcome_unknown:transport_failure");

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, StopCancelsLongQueueCheckTimer) {
  boost::asio::io_context ioc;
  auto manager = std::make_shared<bridge::RetryQueueManager>(
      ioc, bridge::RetryQueuePolicy{.message_retry_base_interval_sec = 1,
                                    .media_retry_base_interval_sec = 1,
                                    .retry_queue_check_interval_sec = 300,
                                    .max_retry_interval_sec = 300});
  manager->start();

  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{10});
  stop_timer.async_wait(
      [manager](const boost::system::error_code &) { manager->stop(); });
  const auto started = std::chrono::steady_clock::now();
  ioc.run();

  EXPECT_EQ(manager->stopped().wait_for(std::chrono::seconds{0}),
            std::future_status::ready);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds{1});
}

TEST(RetryQueueManagerTest, StopWaitsForInFlightResendToRetire) {
  const auto db_path = temp_db_path("stop_in_flight_resend");
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_retry(ready_retry("qq-in-flight")));

  boost::asio::io_context ioc;
  auto manager = std::make_shared<bridge::RetryQueueManager>(ioc, repository);
  manager->restore_persisted_message_retries();
  bool callback_started = false;
  bool callback_completed = false;
  manager->register_message_send_callback(
      "telegram",
      [&callback_started, &callback_completed](
          const bridge::MessageRetryEntry &, const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        callback_started = true;
        boost::asio::steady_timer suspended(
            co_await boost::asio::this_coro::executor);
        suspended.expires_after(std::chrono::milliseconds{50});
        co_await suspended.async_wait(boost::asio::use_awaitable);
        callback_completed = true;
        co_return bridge::MessageSendOutcome::retryable("test failure");
      });
  manager->start();

  bool stopped_was_pending = false;
  boost::asio::steady_timer stop_timer(ioc);
  stop_timer.expires_after(std::chrono::milliseconds{10});
  stop_timer.async_wait([&](const boost::system::error_code &) {
    manager->stop();
    stopped_was_pending =
        manager->stopped().wait_for(std::chrono::seconds{0}) ==
        std::future_status::timeout;
  });
  ioc.run();

  EXPECT_TRUE(callback_started);
  EXPECT_TRUE(stopped_was_pending);
  EXPECT_TRUE(callback_completed);
  EXPECT_EQ(manager->stopped().wait_for(std::chrono::seconds{0}),
            std::future_status::ready);
  EXPECT_EQ(manager->get_pending_message_retry_count(), 0);
  EXPECT_EQ(repository
                ->get_pending_message_retries(std::chrono::system_clock::now() +
                                                  std::chrono::hours{1},
                                              10)
                .size(),
            1);

  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest,
     UnavailableTargetInstallationTerminalizesWithoutPlatformFallback) {
  const auto db_path = temp_db_path("unavailable_installation");
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();
  auto retry = ready_retry("unavailable");
  retry.target_installation = "tg-missing";
  ASSERT_TRUE(repository->add_message_retry(retry));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  int fallback_calls = 0;
  manager.register_message_send_callback(
      "tg-other",
      [&fallback_calls](const bridge::MessageRetryEntry &,
                        const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        ++fallback_calls;
        co_return bridge::MessageSendOutcome::completed("wrong-target");
      });
  manager.start();
  boost::asio::steady_timer stop(ioc);
  stop.expires_after(std::chrono::milliseconds{50});
  stop.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(fallback_calls, 0);
  EXPECT_TRUE(repository
                  ->get_pending_message_retries(
                      std::chrono::system_clock::time_point::max(), 10)
                  .empty());
  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest,
     CollidingNativeRetriesDispatchAndPersistByExactInstallation) {
  const auto db_path = temp_db_path("installation_collisions");
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  auto first = ready_retry("same-native");
  first.source_installation = "qq-a";
  first.target_installation = "tg-a";
  auto second = first;
  second.source_installation = "qq-b";
  second.target_installation = "tg-b";
  ASSERT_TRUE(repository->add_message_retry(first));
  ASSERT_TRUE(repository->add_message_retry(second));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  int first_calls = 0;
  int second_calls = 0;
  manager.register_message_send_callback(
      "tg-a",
      [&first_calls](const bridge::MessageRetryEntry &entry,
                     const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        EXPECT_EQ(entry.source_installation, "qq-a");
        EXPECT_EQ(entry.target_installation, "tg-a");
        ++first_calls;
        co_return bridge::MessageSendOutcome::completed("target-a");
      });
  manager.register_message_send_callback(
      "tg-b",
      [&second_calls](const bridge::MessageRetryEntry &entry,
                      const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        EXPECT_EQ(entry.source_installation, "qq-b");
        EXPECT_EQ(entry.target_installation, "tg-b");
        ++second_calls;
        co_return bridge::MessageSendOutcome::completed("target-b");
      });
  manager.start();
  boost::asio::steady_timer stop(ioc);
  stop.expires_after(std::chrono::milliseconds{50});
  stop.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(first_calls, 1);
  EXPECT_EQ(second_calls, 1);
  EXPECT_EQ(mapped_target(*repository, "qq-a", "group:qq-group", "same-native",
                          "tg-a", "chat:tg-group"),
            "target-a");
  EXPECT_EQ(mapped_target(*repository, "qq-b", "group:qq-group", "same-native",
                          "tg-b", "chat:tg-group"),
            "target-b");
  EXPECT_TRUE(repository
                  ->get_pending_message_retries(
                      std::chrono::system_clock::time_point::max(), 10)
                  .empty());
  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest,
     CollidingNativeRetriesRemainIndependentByConversation) {
  const auto db_path = temp_db_path("conversation_collisions");
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      db_manager, "main", "bridge");
  repository->initialize_schema();

  auto first = ready_retry("same-native");
  first.source_conversation_id = "group:qq-a";
  first.source_group_id = "qq-a";
  first.target_conversation_id = "chat:tg-a";
  first.group_id = "tg-a";
  auto second = first;
  second.source_conversation_id = "group:qq-b";
  second.source_group_id = "qq-b";
  second.target_conversation_id = "chat:tg-b";
  second.group_id = "tg-b";
  ASSERT_TRUE(repository->add_message_retry(first));
  ASSERT_TRUE(repository->add_message_retry(second));

  boost::asio::io_context ioc;
  bridge::RetryQueueManager manager(ioc, repository);
  manager.restore_persisted_message_retries();
  int calls = 0;
  manager.register_message_send_callback(
      "telegram",
      [&calls](const bridge::MessageRetryEntry &entry,
               const obcx::common::Message &)
          -> boost::asio::awaitable<bridge::MessageSendOutcome> {
        ++calls;
        co_return bridge::MessageSendOutcome::completed("target-" +
                                                        entry.group_id);
      });
  manager.start();
  boost::asio::steady_timer stop(ioc);
  stop.expires_after(std::chrono::milliseconds{50});
  stop.async_wait([&](const boost::system::error_code &) {
    manager.stop();
    ioc.stop();
  });
  ioc.run();

  EXPECT_EQ(calls, 2);
  EXPECT_EQ(mapped_target(*repository, "qq", "group:qq-a", "same-native",
                          "telegram", "chat:tg-a"),
            "target-tg-a");
  EXPECT_EQ(mapped_target(*repository, "qq", "group:qq-b", "same-native",
                          "telegram", "chat:tg-b"),
            "target-tg-b");
  EXPECT_TRUE(repository
                  ->get_pending_message_retries(
                      std::chrono::system_clock::time_point::max(), 10)
                  .empty());
  std::filesystem::remove(db_path);
}

TEST(RetryQueueManagerTest, RepeatedWorkerLifecyclesFullyStop) {
  for (int cycle = 0; cycle < 5; ++cycle) {
    boost::asio::io_context ioc;
    auto manager = std::make_shared<bridge::RetryQueueManager>(ioc);
    manager->start();

    boost::asio::steady_timer stop_timer(ioc);
    stop_timer.expires_after(std::chrono::milliseconds{1});
    stop_timer.async_wait(
        [manager](const boost::system::error_code &) { manager->stop(); });
    ioc.run();

    EXPECT_EQ(manager->stopped().wait_for(std::chrono::seconds{0}),
              std::future_status::ready)
        << "cycle " << cycle;
  }
}

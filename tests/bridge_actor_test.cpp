#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "common/config_loader.hpp"
#include "core/db_manager.hpp"
#include "core/native_actor_scheduler.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace asio = boost::asio;

namespace {

auto run_actor(std::shared_ptr<obcx::core::ActorServices> services,
               obcx::core::MessageEnvelope message)
    -> obcx::core::ActorResult {
  using namespace std::chrono_literals;

  asio::io_context ioc;
  services->register_service<asio::any_io_executor>(
      std::make_shared<asio::any_io_executor>(ioc.get_executor()));
  auto work = asio::make_work_guard(ioc);
  std::jthread io_thread([&ioc] { ioc.run(); });

  obcx::core::NativeActorScheduler scheduler(
      obcx::core::NativeActorSchedulerOptions{.worker_count = 2}, services);
  scheduler.register_actor(std::make_shared<bridge::BridgeActor>());
  std::promise<obcx::core::ActorResult> completion;
  auto future = completion.get_future();
  if (!scheduler.enqueue(
          obcx::core::ActorInvocation{.actor_id = "bridge",
                                      .partition_key = "test",
                                      .db_instance = "main",
                                      .db_namespace = "bridge",
                                      .message = std::move(message)},
          [&completion](obcx::core::ActorResult result) {
            completion.set_value(std::move(result));
          })) {
    throw std::runtime_error("native bridge scheduler rejected invocation");
  }
  if (future.wait_for(5s) != std::future_status::ready) {
    scheduler.shutdown(obcx::core::ActorExecutorShutdownMode::Cancel);
    throw std::runtime_error("native bridge invocation timed out");
  }
  auto result = future.get();
  scheduler.shutdown();
  work.reset();
  ioc.stop();
  return result;
}

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("obcx_bridge_actor_" + name + "_" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
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

auto message_stored(const std::string &source_message_id,
                    const std::string &target_message_id)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "stored-" + source_message_id;
  envelope.type = "MessageStored";
  envelope.source_platform = "qq";
  envelope.source_bot = "qq-main";
  envelope.correlation_id = "corr-" + source_message_id;
  envelope.payload = {
      {"message_id", source_message_id},        {"group_id", "group-7"},
      {"target_platform", "telegram"},          {"target_bot", "tg-main"},
      {"target_message_id", target_message_id},
  };
  return envelope;
}

} // namespace

class RecordingForwarder final : public bridge::IBridgeForwarder {
public:
  explicit RecordingForwarder(bridge::BridgeForwardResult result)
      : result_(std::move(result)) {}

  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    seen_messages.push_back(message);
    co_return result_;
  }

  std::vector<obcx::core::MessageEnvelope> seen_messages;

private:
  bridge::BridgeForwardResult result_;
};

class SuspendingForwarder final : public bridge::IBridgeForwarder {
public:
  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor, std::chrono::milliseconds(20));
    co_await timer.async_wait(asio::use_awaitable);
    seen_message = message;
    co_return bridge::BridgeForwardResult{
        .source_platform = "qq",
        .source_message_id = "qq-media-1",
        .target_platform = "telegram",
        .target_bot = "tg-main",
        .target_message_id = "tg-media-1",
    };
  }

  std::optional<obcx::core::MessageEnvelope> seen_message;
};

class ThrowingForwarder final : public bridge::IBridgeForwarder {
public:
  auto forward_message(const obcx::core::MessageEnvelope &)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    throw std::runtime_error("simulated forwarding failure");
    co_return bridge::BridgeForwardResult{};
  }
};

class HangingForwarder final : public bridge::IBridgeForwarder {
public:
  auto started() -> std::future<void> { return started_.get_future(); }

  auto forward_message(const obcx::core::MessageEnvelope &)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    started_.set_value();
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor, std::chrono::seconds(30));
    co_await timer.async_wait(asio::use_awaitable);
    resumed_after_wait.store(true, std::memory_order_release);
    co_return bridge::BridgeForwardResult{};
  }

  std::atomic_bool resumed_after_wait = false;

private:
  std::promise<void> started_;
};

TEST(BridgeActorTest, PersistsMappingAndEmitsMessageForwarded) {
  const auto db_path = temp_db_path("forwarded");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);

  const auto result =
      run_actor(services, message_stored("qq-7", "tg-9"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["source_message_id"], "qq-7");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "tg-9");

  const auto target_message_id = db_manager->run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            "SELECT target_message_id FROM bridge_message_mappings "
            "WHERE source_platform = ? AND source_message_id = ? "
            "AND target_platform = ?;",
            {std::string{"qq"}, std::string{"qq-7"}, std::string{"telegram"}});
        EXPECT_EQ(rows.size(), 1);
        return std::get<std::string>(rows.at(0).at("target_message_id"));
      });
  EXPECT_EQ(target_message_id, "tg-9");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, EmitsMessageForwardFailedWhenMappingFieldsAreMissing) {
  const auto db_path = temp_db_path("failed");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);

  auto stored = message_stored("qq-9", "tg-9");
  stored.payload.erase("target_message_id");

  const auto result = run_actor(services, std::move(stored));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "missing_forward_mapping");
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "MessageForwardFailed");
  EXPECT_EQ(result.emitted.front().payload["code"], "missing_forward_mapping");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, ForwardsMessageStoredThroughRuntimeForwarder) {
  const auto db_path = temp_db_path("runtime_forwarder");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  auto forwarder = std::make_shared<RecordingForwarder>(
      bridge::BridgeForwardResult{.source_platform = "qq",
                                  .source_message_id = "qq-actor-1",
                                  .target_platform = "telegram",
                                  .target_bot = "tg-main",
                                  .target_message_id = "tg-actor-9"});
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-actor-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");

  const auto result = run_actor(services, std::move(stored));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(forwarder->seen_messages.size(), 1);
  EXPECT_EQ(forwarder->seen_messages.front().type, "MessageStored");
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["source_message_id"], "qq-actor-1");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "tg-actor-9");

  const auto target_message_id = db_manager->run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            "SELECT target_message_id FROM bridge_message_mappings "
            "WHERE source_platform = ? AND source_message_id = ? "
            "AND target_platform = ?;",
            {std::string{"qq"}, std::string{"qq-actor-1"},
             std::string{"telegram"}});
        EXPECT_EQ(rows.size(), 1);
        return std::get<std::string>(rows.at(0).at("target_message_id"));
      });
  EXPECT_EQ(target_message_id, "tg-actor-9");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, PreservesMediaPayloadAcrossActorAsioSuspension) {
  const auto db_path = temp_db_path("media_suspension");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  auto forwarder = std::make_shared<SuspendingForwarder>();
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-media-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  stored.raw = {
      {"message",
       {{{"type", "image"},
         {"data", {{"file", "photo.jpg"}, {"url", "https://example.test/photo.jpg"}}}}}},
  };

  const auto result = run_actor(services, stored);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(forwarder->seen_message.has_value());
  EXPECT_EQ(forwarder->seen_message->raw, stored.raw);
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().payload["target_message_id"],
            "tg-media-1");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, ConvertsForwardingExceptionIntoRetryableFailure) {
  const auto db_path = temp_db_path("forward_failure");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<bridge::IBridgeForwarder>(
      std::make_shared<ThrowingForwarder>());

  const auto result =
      run_actor(services, message_stored("qq-failure-1", "unused-target"));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "bridge_error");
  EXPECT_TRUE(result.failure->retryable);
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "MessageForwardFailed");
  EXPECT_EQ(result.emitted.front().payload["retryable"], true);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, ShutdownCancelsSuspendedForwardingWithoutLateResume) {
  using namespace std::chrono_literals;

  const auto db_path = temp_db_path("shutdown");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  asio::io_context ioc;
  auto work = asio::make_work_guard(ioc);
  std::jthread io_thread([&ioc] { ioc.run(); });

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<asio::any_io_executor>(
      std::make_shared<asio::any_io_executor>(ioc.get_executor()));
  auto forwarder = std::make_shared<HangingForwarder>();
  auto started = forwarder->started();
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  obcx::core::NativeActorScheduler scheduler(
      obcx::core::NativeActorSchedulerOptions{.worker_count = 1}, services);
  scheduler.register_actor(std::make_shared<bridge::BridgeActor>());

  std::promise<obcx::core::ActorResult> completion;
  auto completed = completion.get_future();
  ASSERT_TRUE(scheduler.enqueue(
      obcx::core::ActorInvocation{
          .actor_id = "bridge",
          .partition_key = "shutdown",
          .db_instance = "main",
          .db_namespace = "bridge",
          .message = message_stored("qq-shutdown-1", "unused-target")},
      [&completion](obcx::core::ActorResult result) {
        completion.set_value(std::move(result));
      }));
  ASSERT_EQ(started.wait_for(2s), std::future_status::ready);

  scheduler.shutdown(obcx::core::ActorExecutorShutdownMode::Cancel);
  ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
  const auto result = completed.get();
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "scheduler_cancelled");

  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(forwarder->resumed_after_wait.load(std::memory_order_acquire));
  work.reset();
  ioc.stop();
  std::filesystem::remove(db_path);
}

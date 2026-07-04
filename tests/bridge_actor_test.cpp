#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "common/config_loader.hpp"
#include "core/db_manager.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace asio = boost::asio;

namespace {

template <typename T>
auto run_awaitable(asio::io_context &ioc, asio::awaitable<T> awaitable) -> T {
  std::optional<T> result;
  std::exception_ptr exception;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        try {
          result = co_await std::move(awaitable);
        } catch (...) {
          exception = std::current_exception();
        }
      },
      asio::detached);

  ioc.run();
  ioc.restart();

  if (exception) {
    std::rethrow_exception(exception);
  }
  return std::move(*result);
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

TEST(BridgeActorTest, PersistsMappingAndEmitsMessageForwarded) {
  const auto db_path = temp_db_path("forwarded");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);

  bridge::BridgeActor actor;
  obcx::core::ActorContext context("bridge", services, "main", "bridge");

  asio::io_context ioc;
  const auto result = run_awaitable(
      ioc, actor.handle_message(message_stored("qq-7", "tg-9"), context));

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

  bridge::BridgeActor actor;
  obcx::core::ActorContext context("bridge", services, "main", "bridge");

  auto stored = message_stored("qq-9", "tg-9");
  stored.payload.erase("target_message_id");

  asio::io_context ioc;
  const auto result = run_awaitable(ioc, actor.handle_message(stored, context));

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

  bridge::BridgeActor actor;
  obcx::core::ActorContext context("bridge", services, "main", "bridge");
  auto stored = message_stored("qq-actor-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");

  asio::io_context ioc;
  const auto result = run_awaitable(ioc, actor.handle_message(stored, context));

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

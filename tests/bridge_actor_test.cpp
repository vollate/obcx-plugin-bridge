#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
#include "core/blocking_executor.hpp"
#include "core/db_manager.hpp"
#include "core/native_actor_scheduler.hpp"
#include "qq/message_formatter.hpp"
#include "qq/photo_normalizer.hpp"
#include "telegram/handler.hpp"
#include "telegram/media_group_buffer.hpp"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace asio = boost::asio;

namespace {

auto run_actor(std::shared_ptr<obcx::core::ActorServices> services,
               obcx::core::MessageEnvelope message) -> obcx::core::ActorResult {
  using namespace std::chrono_literals;

  asio::io_context ioc;
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(2);
  services->register_service<obcx::core::BlockingExecutor>(blocking_executor);
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
  blocking_executor->shutdown();
  work.reset();
  ioc.stop();
  return result;
}

auto run_actor_sequence(std::shared_ptr<obcx::core::ActorServices> services,
                        std::vector<obcx::core::MessageEnvelope> messages)
    -> std::vector<obcx::core::ActorResult> {
  using namespace std::chrono_literals;

  asio::io_context ioc;
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(2);
  services->register_service<obcx::core::BlockingExecutor>(blocking_executor);
  services->register_service<asio::any_io_executor>(
      std::make_shared<asio::any_io_executor>(ioc.get_executor()));
  auto work = asio::make_work_guard(ioc);
  std::jthread io_thread([&ioc] { ioc.run(); });

  obcx::core::NativeActorScheduler scheduler(
      obcx::core::NativeActorSchedulerOptions{.worker_count = 2}, services);
  scheduler.register_actor(std::make_shared<bridge::BridgeActor>());

  std::vector<obcx::core::ActorResult> results;
  results.reserve(messages.size());
  for (auto &message : messages) {
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
    results.push_back(future.get());
  }

  scheduler.shutdown();
  blocking_executor->shutdown();
  work.reset();
  ioc.stop();
  return results;
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

auto preserve_test_images(const bridge::BridgeConfig &,
                          std::vector<bridge::qq::DownloadedImage> images)
    -> std::vector<bridge::qq::PhotoNormalizationResult> {
  std::vector<bridge::qq::PhotoNormalizationResult> results;
  results.reserve(images.size());
  for (auto &image : images) {
    results.push_back({.image = std::move(image)});
  }
  return results;
}

auto message_stored(const std::string &source_message_id,
                    const std::string &target_message_id)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "stored-" + source_message_id;
  envelope.type = "obcx::message_store::events::MessageStored";
  envelope.source_platform = "qq";
  envelope.source_bot = "qq-main";
  envelope.correlation_id = "corr-" + source_message_id;
  envelope.conversation_id = "group:group-7";
  envelope.payload = {
      {"message_id", source_message_id},
      {"group_id", "group-7"},
      {"target_platform", "telegram"},
      {"target_bot", "tg-main"},
      {"target_conversation_id", "chat:tg-group"},
      {"target_message_id", target_message_id},
  };
  return envelope;
}

template <typename Command>
auto command_message(std::string name, std::string platform = "telegram")
    -> obcx::core::MessageEnvelope {
  Command request;
  request.invocation = {
      .transaction_id = "command-1",
      .name = std::move(name),
      .arguments = {},
      .source_message_id = "source-command-1",
      .source_platform = std::move(platform),
      .source_bot = "primary",
      .conversation_id = "group:42",
      .sender = "7",
      .source_event =
          {
              {"message_id", "source-command-1"},
              {"sender", "7"},
              {"group_id", "42"},
              {"message_type", "group"},
              {"payload", {{"raw_message", "/checkalive"}}},
          },
  };
  obcx::core::MessageEnvelope envelope;
  envelope.id = "command-request-1";
  envelope.type = obcx::core::canonical_message_type_name<Command>();
  envelope.source_platform = request.invocation.source_platform;
  envelope.source_bot = request.invocation.source_bot;
  envelope.conversation_id = request.invocation.conversation_id;
  envelope.payload = request;
  envelope.headers = {
      {std::string{obcx::command::transaction_header}, "command-1"},
      {std::string{obcx::command::actor_header}, "bridge"},
      {std::string{obcx::command::generation_header}, "1"},
      {std::string{obcx::command::reply_header}, "coordinator"},
  };
  return envelope;
}

auto raw_poke_notice() -> obcx::core::MessageEnvelope {
  obcx::core::events::RawNoticeEvent notice{
      .payload =
          {
              {"notice_type", "notify"},
              {"sender", "user-7"},
              {"group_id", "qq-group"},
              {"payload", {{"sub_type", "poke"}, {"target_id", 80008}}},
          },
  };
  obcx::core::MessageEnvelope envelope;
  envelope.id = "notice-qq-poke";
  envelope.type = obcx::core::canonical_message_type_name<decltype(notice)>();
  envelope.source_platform = "qq";
  envelope.source_bot = "qq-main";
  envelope.conversation_id = "group:qq-group";
  envelope.payload = notice;
  envelope.raw = {
      {"time", 1720000000.456}, {"self_id", 90001},
      {"post_type", "notice"},  {"notice_type", "notify"},
      {"sub_type", "poke"},     {"user_id", 70007},
      {"target_id", 80008},     {"group_id", "qq-group"},
  };
  return envelope;
}

auto installation_for(const std::string_view platform) -> std::string {
  return platform == "qq" ? "qq-main" : "tg-main";
}

auto mapped_target(bridge::BridgeStateRepository &repository,
                   const std::string &source_platform,
                   const std::string &source_message_id,
                   const std::string &target_platform)
    -> std::optional<std::string> {
  const auto result = repository.resolve_target_mapping(
      {.installation_id = installation_for(source_platform),
       .platform = source_platform,
       .conversation_id =
           source_platform == "qq" ? "group:qq-group" : "chat:tg-group",
       .message_id = source_message_id},
      {.installation_id = installation_for(target_platform),
       .platform = target_platform,
       .conversation_id =
           target_platform == "qq" ? "group:qq-group" : "chat:tg-group"});
  return result.unique()
             ? std::optional<std::string>{result.mapping->target_message_id}
             : std::nullopt;
}

} // namespace

TEST(BridgeActorTest, LoadsGroupMappingsFromGenerationSnapshot) {
  const auto config_path =
      temp_db_path("actor_config").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[bots.qq-main]
enabled = true
surface = "onebot11.qq"
transport = "http"
[bots.qq-main.connection]

[bots.tg-main]
enabled = true
surface = "telegram.bot_api"
transport = "http"
[bots.tg-main.connection]
access_token = "YOUR_TELEGRAM_TOKEN"

[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"

[[group_mappings.group_to_group]]
telegram_group_id = "tg-group"
qq_group_id = "qq-group"
mode = "group_to_group"
show_qq_to_tg_sender = true
show_tg_to_qq_sender = false
enable_qq_to_tg = true
enable_tg_to_qq = true
)";
  }

  auto built = obcx::common::ConfigLoader::build_snapshot(config_path.string());
  ASSERT_TRUE(built);
  const auto config = bridge::load_bridge_config(
      obcx::common::ActorConfigView{built.snapshot, "bridge"});

  const auto mapping = config->group_map.find("tg-group");
  ASSERT_NE(mapping, config->group_map.end());
  EXPECT_EQ(mapping->second.qq_group_id, "qq-group");
  EXPECT_TRUE(mapping->second.enable_qq_to_tg);
  EXPECT_FALSE(mapping->second.show_tg_to_qq_sender);

  std::filesystem::remove(config_path);
}

class RecordingForwarder final : public bridge::IBridgeForwarder {
public:
  explicit RecordingForwarder(bridge::BridgeForwardResult result,
                              const bool infer_conversations = true)
      : result_(std::move(result)), infer_conversations_(infer_conversations) {}

  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    seen_messages.push_back(message);
    auto result = result_;
    if (infer_conversations_ && result.source_conversation_id.empty()) {
      result.source_conversation_id = message.conversation_id;
    }
    if (infer_conversations_ && result.target_conversation_id.empty() &&
        !result.target_platform.empty()) {
      result.target_conversation_id =
          result.target_platform == "qq" ? "group:qq-group" : "chat:tg-group";
    }
    co_return result;
  }

  auto handle_command(const obcx::command::CommandInvocation &invocation)
      -> asio::awaitable<bool> override {
    seen_commands.push_back(invocation);
    co_return true;
  }

  auto handle_notice(const obcx::core::MessageEnvelope &notice)
      -> asio::awaitable<bool> override {
    seen_notices.push_back(notice);
    co_return true;
  }

  std::vector<obcx::core::MessageEnvelope> seen_messages;
  std::vector<obcx::core::MessageEnvelope> seen_notices;
  std::vector<obcx::command::CommandInvocation> seen_commands;

private:
  bridge::BridgeForwardResult result_;
  bool infer_conversations_ = true;
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
        .disposition = bridge::DirectForwardDisposition::NewDelivery,
        .source_platform = "qq",
        .source_bot = "qq-main",
        .source_conversation_id = "group:qq-group",
        .source_message_id = "qq-media-1",
        .target_platform = "telegram",
        .target_bot = "tg-main",
        .target_conversation_id = "chat:tg-group",
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

TEST(BridgeActorTest, DeclaresTypedCommandsWithoutHandlerMetadata) {
  const auto contract =
      obcx::common::json::parse(bridge::BridgeActor::input_contract_json());
  ASSERT_TRUE(contract.contains("commands"));
  ASSERT_EQ(contract["commands"].size(), 3U);
  EXPECT_EQ(contract["commands"][0]["name"], "checkalive");
  EXPECT_EQ(contract["commands"][1]["name"], "poke");
  EXPECT_EQ(contract["commands"][2]["name"], "recall");
  EXPECT_FALSE(contract["commands"][0].contains("handler"));
  EXPECT_FALSE(contract["commands"][0].contains("callable"));
  ASSERT_TRUE(contract.contains("configuration"));
  EXPECT_EQ(contract["configuration"]["required_strings"],
            obcx::common::json::array({"bridge_files_dir"}));
  EXPECT_EQ(contract["configuration"]["bot_installations"]
                    ["telegram_installation"]["types"],
            "telegram");
  EXPECT_EQ(contract["configuration"]["bot_installations"]
                    ["onebot11_installation"]["types"],
            "qq");
  const auto &pairs = contract["configuration"]["bot_installation_collections"]
                              ["installation_pairs"];
  EXPECT_EQ(pairs["identity"], "id");
  EXPECT_EQ(pairs["minimum_items"], 1);
  EXPECT_EQ(pairs["bot_installations"]["telegram_installation"], "telegram");
  EXPECT_TRUE(std::ranges::any_of(
      contract["configuration"]["collection_identity_references"],
      [](const auto &reference) {
        return reference.value("root_section", std::string{}) ==
                   "actors.bridge.config" &&
               reference.value("source_key", std::string{}) == "pair";
      }));
  EXPECT_TRUE(
      std::ranges::any_of(contract["accepted_inputs"], [](const auto &input) {
        return input == "obcx::core::events::RawNoticeEvent";
      }));
}

TEST(BridgeActorTest, HandlesTypedCommandAndReturnsContinueCompletion) {
  auto services = std::make_shared<obcx::core::ActorServices>();
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{});
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  const auto result = run_actor(
      services,
      command_message<bridge::commands::CheckAliveCommand>("checkalive"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(forwarder->seen_commands.size(), 1U);
  EXPECT_EQ(forwarder->seen_commands.front().name, "checkalive");
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type,
            obcx::core::canonical_message_type_name<
                obcx::command::CommandCompleted>());
  const auto completion =
      result.emitted.front().payload.get<obcx::command::CommandCompleted>();
  EXPECT_EQ(completion.transaction_id, "command-1");
  EXPECT_EQ(completion.propagation, obcx::command::Propagation::Continue);
}

TEST(BridgeActorTest, HandlesEveryConfiguredPlatformCommandAsTypedMessage) {
  auto services = std::make_shared<obcx::core::ActorServices>();
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{});
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  EXPECT_TRUE(
      run_actor(services,
                command_message<bridge::commands::RecallCommand>("recall"))
          .ok());
  EXPECT_TRUE(run_actor(services,
                        command_message<bridge::commands::PokeCommand>("poke"))
                  .ok());
  EXPECT_TRUE(
      run_actor(services, command_message<bridge::commands::CheckAliveCommand>(
                              "checkalive", "qq"))
          .ok());

  ASSERT_EQ(forwarder->seen_commands.size(), 3U);
  EXPECT_EQ(forwarder->seen_commands[0].source_platform, "telegram");
  EXPECT_EQ(forwarder->seen_commands[0].name, "recall");
  EXPECT_EQ(forwarder->seen_commands[1].source_platform, "telegram");
  EXPECT_EQ(forwarder->seen_commands[1].name, "poke");
  EXPECT_EQ(forwarder->seen_commands[2].source_platform, "qq");
  EXPECT_EQ(forwarder->seen_commands[2].name, "checkalive");
}

TEST(BridgeActorTest, RoutesTypedNoticeThroughActorForwarder) {
  auto services = std::make_shared<obcx::core::ActorServices>();
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{});
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  const auto result = run_actor(services, raw_poke_notice());

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(forwarder->seen_notices.size(), 1U);
  EXPECT_EQ(forwarder->seen_notices.front().source_platform, "qq");
  EXPECT_EQ(forwarder->seen_notices.front().raw["sub_type"], "poke");
  EXPECT_TRUE(result.emitted.empty());
}

TEST(BridgeActorTest, ProcessedStoredCommandIsNotForwardedOrMappedAgain) {
  auto services = std::make_shared<obcx::core::ActorServices>();
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{});
  services->register_service<bridge::IBridgeForwarder>(forwarder);
  auto stored = message_stored("command-source", "");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  stored.headers.emplace(std::string{obcx::command::processed_header}, "true");

  const auto result = run_actor(services, std::move(stored));

  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(result.emitted.empty());
  EXPECT_TRUE(forwarder->seen_messages.empty());
}

TEST(BridgeActorTest, PersistsMappingAndEmitsMessageForwarded) {
  const auto db_path = temp_db_path("forwarded");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);

  const auto result = run_actor(services, message_stored("qq-7", "tg-9"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
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
  EXPECT_EQ(result.emitted.front().type,
            "bridge::events::MessageForwardFailed");
  EXPECT_EQ(result.emitted.front().payload["code"], "missing_forward_mapping");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, UnmappedMessageIsSuccessfulNoOp) {
  const auto db_path = temp_db_path("runtime_forwarder_noop");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{});
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-unmapped-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  const auto result = run_actor(services, std::move(stored));

  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(result.emitted.empty());
  ASSERT_EQ(forwarder->seen_messages.size(), 1U);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, DeliveryFailureKeepsTypedDiagnosticWithoutMapping) {
  const auto db_path = temp_db_path("runtime_forwarder_delivery_failure");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{
          .disposition = bridge::DirectForwardDisposition::DeliveryFailed,
          .source_platform = "qq",
          .source_bot = "qq-main",
          .source_message_id = "qq-failed-1",
          .target_platform = "telegram",
          .target_bot = "tg-main",
          .failure_message = "transport_failure",
          .failure_retryable = true,
      });
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-failed-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  const auto result = run_actor(services, std::move(stored));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "bridge_delivery_failed");
  EXPECT_EQ(result.failure->message, "transport_failure");
  EXPECT_TRUE(result.failure->retryable);
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type,
            "bridge::events::MessageForwardFailed");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, ForwardsMessageStoredThroughRuntimeForwarder) {
  const auto db_path = temp_db_path("runtime_forwarder");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{
          .disposition = bridge::DirectForwardDisposition::NewDelivery,
          .source_platform = "qq",
          .source_bot = "qq-main",
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
  EXPECT_EQ(forwarder->seen_messages.front().type,
            "obcx::message_store::events::MessageStored");
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
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

TEST(BridgeActorTest, RejectsIncompleteDirectForwardOutcomeWithoutWriting) {
  const auto db_path = temp_db_path("incomplete_direct_outcome");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<bridge::BridgeStateRepository>(repository);
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{
          .disposition = bridge::DirectForwardDisposition::NewDelivery,
          .source_platform = "qq",
          .source_bot = "qq-main",
          .source_message_id = "qq-incomplete-1",
          .target_platform = "telegram",
          .target_bot = "tg-main",
      });
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-incomplete-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  const auto result = run_actor(services, std::move(stored));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "missing_forward_mapping");
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type,
            "bridge::events::MessageForwardFailed");
  EXPECT_EQ(
      repository->message_mapping_operation_counts().direct_forward_writes, 0U);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, RejectsDeliveredOutcomeWithoutConversations) {
  const auto db_path = temp_db_path("missing_conversations");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();
  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<bridge::BridgeStateRepository>(repository);
  services->register_service<bridge::IBridgeForwarder>(
      std::make_shared<RecordingForwarder>(
          bridge::BridgeForwardResult{
              .disposition = bridge::DirectForwardDisposition::NewDelivery,
              .source_platform = "qq",
              .source_bot = "qq-main",
              .source_message_id = "source",
              .target_platform = "telegram",
              .target_bot = "tg-main",
              .target_message_id = "target"},
          false));

  const auto result = run_actor(services, message_stored("source", "unused"));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "missing_forward_mapping");
  EXPECT_EQ(
      repository->message_mapping_operation_counts().direct_forward_writes, 0U);
  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest,
     MappingPersistenceFailureEmitsFailureWithoutResendingOrForwardedEvent) {
  const auto db_path = temp_db_path("direct_mapping_failure");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  db_manager->run_write<void>("main",
                              [](obcx::core::IDbConnection &connection) {
                                connection.execute(R"(
          CREATE TRIGGER fail_direct_mapping_insert
          BEFORE INSERT ON bridge_message_mappings
          BEGIN
            SELECT RAISE(FAIL, 'injected direct mapping persistence failure');
          END;
        )");
                              });
  repository->reset_message_mapping_operation_counts();

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<bridge::BridgeStateRepository>(repository);
  auto forwarder =
      std::make_shared<RecordingForwarder>(bridge::BridgeForwardResult{
          .disposition = bridge::DirectForwardDisposition::NewDelivery,
          .source_platform = "qq",
          .source_bot = "qq-main",
          .source_message_id = "qq-persist-failure-1",
          .target_platform = "telegram",
          .target_bot = "tg-main",
          .target_message_id = "tg-uncommitted-1",
      });
  services->register_service<bridge::IBridgeForwarder>(forwarder);

  auto stored = message_stored("qq-persist-failure-1", "unused-target");
  stored.payload.erase("target_platform");
  stored.payload.erase("target_message_id");
  const auto result = run_actor(services, std::move(stored));

  ASSERT_FALSE(result.ok());
  ASSERT_TRUE(result.failure.has_value());
  EXPECT_EQ(result.failure->code, "mapping_persistence_failed");
  EXPECT_FALSE(result.failure->retryable);
  ASSERT_EQ(forwarder->seen_messages.size(), 1U);
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type,
            "bridge::events::MessageForwardFailed");
  EXPECT_EQ(
      repository->message_mapping_operation_counts().direct_forward_writes, 1U);
  EXPECT_FALSE(
      mapped_target(*repository, "qq", "qq-persist-failure-1", "telegram")
          .has_value());

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
         {"data",
          {{"file", "photo.jpg"},
           {"url", "https://example.test/photo.jpg"}}}}}},
  };

  const auto result = run_actor(services, stored);

  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(forwarder->seen_message.has_value());
  EXPECT_EQ(forwarder->seen_message->raw, stored.raw);
  ASSERT_EQ(result.emitted.size(), 1);
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "tg-media-1");

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
  EXPECT_EQ(result.emitted.front().type,
            "bridge::events::MessageForwardFailed");
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
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<obcx::core::BlockingExecutor>(blocking_executor);
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
  blocking_executor->shutdown();
  work.reset();
  ioc.stop();
  std::filesystem::remove(db_path);
}

#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
#include "core/blocking_executor.hpp"
#include "core/bot_registry.hpp"
#include "core/db_manager.hpp"
#include "core/native_actor_scheduler.hpp"
#if __has_include("core/qq_bot.hpp") && __has_include("core/tg_bot.hpp")
#define OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS 1
#include "core/qq_bot.hpp"
#include "core/tg_bot.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "telegram/adapter/protocol_adapter.hpp"
#else
#define OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS 0
#endif

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
  envelope.type = "obcx::message_store::events::MessageStored";
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

#if OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS
auto bridge_message_stored(std::string source_platform,
                           std::string source_message_id, std::string group_id)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "stored-" + source_message_id;
  envelope.type = "obcx::message_store::events::MessageStored";
  envelope.source_platform = std::move(source_platform);
  envelope.source_bot =
      envelope.source_platform == "qq" ? "qq-main" : "tg-main";
  envelope.correlation_id = "corr-" + source_message_id;
  envelope.conversation_id = "group:" + group_id;
  envelope.payload = {
      {"message_id", source_message_id},
      {"conversation_id", envelope.conversation_id},
      {"sender", "sender-1"},
      {"group_id", group_id},
      {"message_type", "group"},
      {"payload", {{"text", "retry actor message"}}},
  };
  envelope.raw = {
      {"post_type", "message"},
      {"message_type", "group"},
      {"sub_type", "normal"},
      {"message_id", source_message_id},
      {"user_id", "sender-1"},
      {"group_id", group_id},
      {"raw_message", "retry actor message"},
      {"message",
       {{{"type", "text"}, {"data", {{"text", "retry actor message"}}}}}},
  };
  return envelope;
}

auto bridge_config_service(const bool enable_retry,
                           const int retry_interval_seconds = 30,
                           const bool topic_mapping = false)
    -> std::shared_ptr<obcx::common::ActorConfigService> {
  const auto config_path =
      temp_db_path(enable_retry ? "retry_enabled" : "retry_disabled")
          .replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << "[actors.bridge.config]\n"
              "bridge_files_dir = \"/tmp/bridge_files\"\n"
              "enable_retry_queue = "
           << (enable_retry ? "true\n" : "false\n")
           << "message_retry_max_attempts = 5\n"
              "message_retry_base_interval_sec = "
           << retry_interval_seconds
           << "\nretry_queue_check_interval_sec = " << retry_interval_seconds
           << "\nmax_retry_interval_sec = "
           << std::max(2, retry_interval_seconds * 2) << "\n\n";
    if (topic_mapping) {
      config << "[[group_mappings.topic_to_group]]\n"
                "telegram_group_id = \"tg-group\"\n"
                "mode = \"topic_to_group\"\n"
                "show_qq_to_tg_sender = false\n"
                "show_tg_to_qq_sender = false\n"
                "enable_qq_to_tg = true\n"
                "enable_tg_to_qq = true\n\n"
                "[[group_mappings.topics]]\n"
                "telegram_group_id = \"tg-group\"\n"
                "telegram_topic_id = 42\n"
                "qq_group_id = \"qq-group\"\n"
                "show_qq_to_tg_sender = false\n"
                "show_tg_to_qq_sender = false\n"
                "enable_qq_to_tg = true\n"
                "enable_tg_to_qq = true\n";
    } else {
      config << "[[group_mappings.group_to_group]]\n"
                "telegram_group_id = \"tg-group\"\n"
                "qq_group_id = \"qq-group\"\n"
                "mode = \"group_to_group\"\n"
                "show_qq_to_tg_sender = false\n"
                "show_tg_to_qq_sender = false\n"
                "enable_qq_to_tg = true\n"
                "enable_tg_to_qq = true\n";
    }
  }
  auto built = obcx::common::ConfigLoader::build_snapshot(config_path.string());
  std::filesystem::remove(config_path);
  if (!built) {
    throw std::runtime_error("failed to build bridge retry test config");
  }
  return std::make_shared<obcx::common::ActorConfigService>(built.snapshot);
}

class RetryTestQQBot final : public obcx::core::QQBot {
public:
  RetryTestQQBot() : QQBot(obcx::adapter::onebot11::ProtocolAdapter{}) {}

  auto send_group_message(std::string_view, const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    const auto call = send_group_calls.fetch_add(1);
    if (succeed_after_first.load(std::memory_order_acquire) && call > 0) {
      co_return "{\"status\":\"ok\",\"data\":{\"message_id\":9001}}";
    }
    co_return "{}";
  }

  auto get_group_member_info(std::string_view, std::string_view user_id, bool)
      -> asio::awaitable<std::string> override {
    co_return "{\"status\":\"ok\",\"data\":{\"user_id\":\"" +
        std::string{user_id} + "\",\"nickname\":\"retry-user\",\"card\":\"\"}}";
  }

  std::atomic_int send_group_calls = 0;
  std::atomic_bool succeed_after_first = false;
};

class RetryTestTelegramBot final : public obcx::core::TGBot {
public:
  RetryTestTelegramBot() : TGBot(obcx::adapter::telegram::ProtocolAdapter{}) {}

  auto send_group_message(std::string_view, const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    const auto call = send_group_calls.fetch_add(1);
    if (succeed_after_first.load(std::memory_order_acquire) && call > 0) {
      co_return "{\"result\":{\"message_id\":8001}}";
    }
    co_return "{}";
  }

  auto send_topic_message(std::string_view, int64_t,
                          const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    const auto call = send_topic_calls.fetch_add(1);
    if (succeed_after_first.load(std::memory_order_acquire) && call > 0) {
      co_return "{\"result\":{\"message_id\":8002}}";
    }
    co_return "{}";
  }

  std::atomic_int send_group_calls = 0;
  std::atomic_int send_topic_calls = 0;
  std::atomic_bool succeed_after_first = false;
};

class NoticeTestTelegramBot final : public obcx::core::TGBot {
public:
  NoticeTestTelegramBot() : TGBot(obcx::adapter::telegram::ProtocolAdapter{}) {}

  auto send_group_message(std::string_view group_id,
                          const obcx::common::Message &message)
      -> asio::awaitable<std::string> override {
    last_group_id = group_id;
    last_message = message;
    send_group_calls.fetch_add(1, std::memory_order_relaxed);
    co_return R"({"ok":true,"result":{"message_id":8001}})";
  }

  std::atomic_int send_group_calls = 0;
  std::string last_group_id;
  obcx::common::Message last_message;
};

auto bridge_retry_services(
    const std::shared_ptr<obcx::core::DbManager> &db_manager,
    const std::shared_ptr<RetryTestQQBot> &qq_bot,
    const std::shared_ptr<RetryTestTelegramBot> &telegram_bot,
    const bool enable_retry, const int retry_interval_seconds = 30,
    const bool topic_mapping = false)
    -> std::shared_ptr<obcx::core::ActorServices> {
  auto registry = std::make_shared<obcx::core::BotRegistry>();
  registry->register_bot("qq", "qq-main", qq_bot);
  registry->register_bot("telegram", "tg-main", telegram_bot);

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<obcx::core::BotRegistry>(registry);
  services->register_service<obcx::common::ActorConfigService>(
      bridge_config_service(enable_retry, retry_interval_seconds,
                            topic_mapping));
  return services;
}

struct LiveRetryResult {
  obcx::core::ActorResult initial_result;
  std::optional<std::string> target_message_id;
};

auto run_actor_until_retry(
    const std::shared_ptr<obcx::core::ActorServices> &services,
    obcx::core::MessageEnvelope message,
    const std::shared_ptr<bridge::BridgeStateRepository> &repository,
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> LiveRetryResult {
  using namespace std::chrono_literals;

  asio::io_context ioc;
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(2);
  services->register_service<obcx::core::BlockingExecutor>(blocking_executor);
  services->register_service<asio::any_io_executor>(
      std::make_shared<asio::any_io_executor>(ioc.get_executor()));
  auto work = asio::make_work_guard(ioc);
  std::jthread io_thread([&ioc] { ioc.run(); });

  LiveRetryResult result;
  {
    obcx::core::NativeActorScheduler scheduler(
        obcx::core::NativeActorSchedulerOptions{.worker_count = 2}, services);
    scheduler.register_actor(std::make_shared<bridge::BridgeActor>());
    std::promise<obcx::core::ActorResult> completion;
    auto future = completion.get_future();
    if (!scheduler.enqueue(
            obcx::core::ActorInvocation{.actor_id = "bridge",
                                        .partition_key = "retry-live",
                                        .db_instance = "main",
                                        .db_namespace = "bridge",
                                        .message = std::move(message)},
            [&completion](obcx::core::ActorResult actor_result) {
              completion.set_value(std::move(actor_result));
            })) {
      throw std::runtime_error("native bridge scheduler rejected invocation");
    }
    if (future.wait_for(5s) != std::future_status::ready) {
      scheduler.shutdown(obcx::core::ActorExecutorShutdownMode::Cancel);
      throw std::runtime_error("native bridge invocation timed out");
    }
    result.initial_result = future.get();

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      result.target_message_id = repository->get_target_message_id(
          source_platform, source_message_id, target_platform);
      if (result.target_message_id.has_value()) {
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    scheduler.shutdown();
  }

  blocking_executor->shutdown();
  work.reset();
  ioc.stop();
  return result;
}
#endif

} // namespace

TEST(BridgeActorTest, LoadsGroupMappingsFromGenerationSnapshot) {
  const auto config_path =
      temp_db_path("actor_config").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[actors.bridge.config]
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
  explicit RecordingForwarder(bridge::BridgeForwardResult result)
      : result_(std::move(result)) {}

  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    seen_messages.push_back(message);
    co_return result_;
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

#if OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS
TEST(BridgeActorTest, ForwardsQqPokeNoticeToTelegramThroughRuntime) {
  const auto db_path = temp_db_path("poke_notice");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<NoticeTestTelegramBot>();
  auto registry = std::make_shared<obcx::core::BotRegistry>();
  registry->register_bot("qq", "qq-main", qq_bot);
  registry->register_bot("telegram", "tg-main", telegram_bot);
  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<obcx::core::BotRegistry>(registry);
  services->register_service<obcx::common::ActorConfigService>(
      bridge_config_service(false));

  const auto result = run_actor(services, raw_poke_notice());

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 1);
  EXPECT_EQ(telegram_bot->last_group_id, "tg-group");
  ASSERT_EQ(telegram_bot->last_message.size(), 1U);
  EXPECT_EQ(telegram_bot->last_message.front().type, "text");
  EXPECT_NE(
      telegram_bot->last_message.front().data["text"].get<std::string>().find(
          "戳了戳"),
      std::string::npos);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, EnabledQqToTelegramFailurePersistsMessageRetry) {
  const auto db_path = temp_db_path("qq_to_tg_retry");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services = bridge_retry_services(db_manager, qq_bot, telegram_bot, true);

  const auto result = run_actor(
      services, bridge_message_stored("qq", "qq-retry-1", "qq-group"));

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  const auto retries = repository->get_pending_message_retries(
      std::chrono::system_clock::now() + std::chrono::hours{1}, 10);
  ASSERT_EQ(retries.size(), 1);
  EXPECT_EQ(retries.front().source_platform, "qq");
  EXPECT_EQ(retries.front().target_platform, "telegram");
  EXPECT_EQ(retries.front().source_message_id, "qq-retry-1");
  EXPECT_EQ(retries.front().group_id, "tg-group");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, EnabledTelegramToQqFailurePersistsMessageRetry) {
  const auto db_path = temp_db_path("tg_to_qq_retry");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services = bridge_retry_services(db_manager, qq_bot, telegram_bot, true);

  const auto result = run_actor(
      services, bridge_message_stored("telegram", "tg-retry-1", "tg-group"));

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  const auto retries = repository->get_pending_message_retries(
      std::chrono::system_clock::now() + std::chrono::hours{1}, 10);
  ASSERT_EQ(retries.size(), 1);
  EXPECT_EQ(retries.front().source_platform, "telegram");
  EXPECT_EQ(retries.front().target_platform, "qq");
  EXPECT_EQ(retries.front().source_message_id, "tg-retry-1");
  EXPECT_EQ(retries.front().group_id, "qq-group");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, DisabledRetryCreatesNoWorkerOrDurableRow) {
  const auto db_path = temp_db_path("retry_disabled");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);

  const auto result = run_actor(
      services, bridge_message_stored("qq", "qq-no-retry-1", "qq-group"));

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramGroupRetryUsesBotRegistryAndPersistsMapping) {
  const auto db_path = temp_db_path("telegram_group_retry_success");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  telegram_bot->succeed_after_first.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, true, 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("qq", "qq-group-retry-success", "qq-group"),
      repository, "qq", "qq-group-retry-success", "telegram");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "8001");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 2);
  EXPECT_EQ(telegram_bot->send_topic_calls.load(), 0);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramTopicRetryUsesTopicApiAndPersistsMapping) {
  const auto db_path = temp_db_path("telegram_topic_retry_success");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  telegram_bot->succeed_after_first.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, true, 1, true);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("qq", "qq-topic-retry-success", "qq-group"),
      repository, "qq", "qq-topic-retry-success", "telegram");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "8002");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);
  EXPECT_EQ(telegram_bot->send_topic_calls.load(), 2);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, QqGroupRetryUsesBotRegistryAndPersistsMapping) {
  const auto db_path = temp_db_path("qq_group_retry_success");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  qq_bot->succeed_after_first.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, true, 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("telegram", "tg-group-retry-success", "tg-group"),
      repository, "telegram", "tg-group-retry-success", "qq");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "9001");
  EXPECT_EQ(qq_bot->send_group_calls.load(), 2);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}
#endif

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

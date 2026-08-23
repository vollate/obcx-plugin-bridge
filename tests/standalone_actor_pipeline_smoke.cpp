#include "bridge_forwarder.hpp"

#include "common/config_loader.hpp"
#include "core/actor_manager.hpp"
#include "core/blocking_executor.hpp"
#include "core/db_manager.hpp"
#include "core/orchestrator.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

class RecordingForwarder final : public bridge::IBridgeForwarder {
public:
  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> asio::awaitable<bridge::BridgeForwardResult> override {
    seen_messages.push_back(message);
    co_return bridge::BridgeForwardResult{
        .disposition = bridge::DirectForwardDisposition::NewDelivery,
        .source_platform = message.source_platform,
        .source_bot = message.source_bot,
        .source_conversation_id = message.conversation_id,
        .source_message_id = message.payload.value("message_id", ""),
        .target_platform = "telegram",
        .target_bot = "tg-conformance",
        .target_conversation_id = "chat:conformance",
        .target_message_id = "tg-conformance-1",
    };
  }

  std::vector<obcx::core::MessageEnvelope> seen_messages;
};

auto stage(std::string name, std::string actor, std::string input,
           std::string output, std::vector<std::string> after = {})
    -> obcx::common::PipelineStageConfig {
  obcx::common::PipelineStageConfig value;
  value.name = std::move(name);
  value.actor = std::move(actor);
  value.input = std::move(input);
  value.outputs = {std::move(output)};
  value.mode = "await";
  value.after = std::move(after);
  return value;
}

auto actor_config(std::string name, std::string db_namespace)
    -> obcx::common::ActorConfig {
  obcx::common::ActorConfig value;
  value.name = std::move(name);
  value.enabled = true;
  value.partition = "source_platform:conversation_id";
  value.db = "main";
  value.db_namespace = std::move(db_namespace);
  return value;
}

auto raw_message(const std::uint64_t sequence) -> obcx::core::MessageEnvelope {
  const auto suffix = std::to_string(sequence);
  obcx::core::MessageEnvelope message;
  message.id = "raw-conformance-" + suffix;
  message.type = "obcx::core::events::RawMessageEvent";
  message.source_platform = "qq";
  message.source_bot = "qq-conformance";
  message.conversation_id = "group:conformance";
  message.correlation_id = "correlation-conformance-" + suffix;
  message.payload = {
      {"message_id", "qq-conformance-" + suffix},
      {"conversation_id", message.conversation_id},
      {"sender", "user-conformance"},
      {"group_id", "conformance"},
      {"message_type", "group"},
      {"payload", {{"text", "actor-only conformance"}}},
  };
  message.raw = {{"raw_message", "actor-only conformance"}};
  return message;
}

void require(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

auto scalar_count(obcx::core::DbManager &manager, const std::string &table)
    -> std::int64_t {
  return manager.run_read<std::int64_t>(
      "main", [&table](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query("SELECT COUNT(*) AS count FROM \"" +
                                           table + "\";");
        if (rows.empty()) {
          return std::int64_t{0};
        }
        return std::get<std::int64_t>(rows.front().at("count"));
      });
}

} // namespace

auto main(int argc, char **argv) -> int {
  using namespace std::chrono_literals;

  if (argc != 3 && argc != 4) {
    std::fprintf(stderr,
                 "usage: %s MESSAGE_STORE_ACTOR BRIDGE_ACTOR [MESSAGES]\n",
                 argv[0]);
    return 1;
  }

  std::uint64_t message_count = 1;
  if (argc == 4) {
    try {
      std::size_t consumed = 0;
      const auto count_text = std::string(argv[3]);
      const auto parsed = std::stoull(count_text, &consumed);
      if (consumed != count_text.size() || parsed == 0 ||
          parsed > static_cast<unsigned long long>(
                       std::numeric_limits<std::int64_t>::max())) {
        throw std::out_of_range("message count must be positive");
      }
      message_count = static_cast<std::uint64_t>(parsed);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "invalid message count: %s\n", error.what());
      return 1;
    }
  }

  const auto database =
      std::filesystem::temp_directory_path() /
      ("obcx_actor_pipeline_conformance_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".sqlite3");

  try {
    obcx::core::ActorManager manager;
    require(manager.load_actor_from_path(argv[1]),
            "failed to load installed message-store actor");
    require(manager.load_actor_from_path(argv[2]),
            "failed to load installed bridge actor");

    auto message_store = manager.get_actor_shared("message_store");
    auto bridge_actor = manager.get_actor_shared("bridge");
    require(message_store != nullptr, "message-store actor was not registered");
    require(bridge_actor != nullptr, "bridge actor was not registered");

    auto db_manager = std::make_shared<obcx::core::DbManager>();
    obcx::common::DbInstanceConfig database_config;
    database_config.name = "main";
    database_config.type = "sqlite";
    database_config.path = database.string();
    db_manager->configure({database_config});

    asio::io_context io_context;
    auto services = std::make_shared<obcx::core::ActorServices>();
    auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(2);
    services->register_service<obcx::core::DbManager>(db_manager);
    services->register_service<obcx::core::BlockingExecutor>(blocking_executor);
    services->register_service<asio::any_io_executor>(
        std::make_shared<asio::any_io_executor>(io_context.get_executor()));
    auto forwarder = std::make_shared<RecordingForwarder>();
    services->register_service<bridge::IBridgeForwarder>(forwarder);

    auto scheduler = std::make_shared<obcx::core::NativeActorScheduler>(
        obcx::core::NativeActorSchedulerOptions{.worker_count = 2}, services);
    obcx::core::Orchestrator orchestrator(scheduler, services);
    orchestrator.register_actor(std::move(message_store));
    orchestrator.register_actor(std::move(bridge_actor));
    orchestrator.configure_actors(
        {actor_config("message_store", "message_store"),
         actor_config("bridge", "bridge")});
    orchestrator.configure_pipelines({obcx::common::PipelineConfig{
        .name = "actor_conformance",
        .source = "obcx::core::events::RawMessageEvent",
        .stages =
            {
                stage("persist", "message_store",
                      "obcx::core::events::RawMessageEvent",
                      "obcx::message_store::events::MessageStored"),
                stage("forward", "bridge",
                      "obcx::message_store::events::MessageStored",
                      "bridge::events::MessageForwarded", {"persist"}),
            },
    }});

    auto work_guard = asio::make_work_guard(io_context);
    std::jthread io_thread([&io_context] { io_context.run(); });

    for (std::uint64_t sequence = 1; sequence <= message_count; ++sequence) {
      const auto input = raw_message(sequence);
      auto future = asio::co_spawn(io_context, orchestrator.process(input),
                                   asio::use_future);
      if (future.wait_for(5s) != std::future_status::ready) {
        const auto forwarded = forwarder->seen_messages.size();
        orchestrator.shutdown();
        work_guard.reset();
        io_context.stop();
        throw std::runtime_error(
            "installed actor pipeline timed out at sequence " +
            std::to_string(sequence) + " after " + std::to_string(forwarded) +
            " forwarded messages");
      }
      const auto result = future.get();

      require(result.ok(), "installed actor pipeline returned a failure");
      require(result.stages.size() == 2,
              "installed actor pipeline did not execute both stages");
      require(result.emitted.size() == 2,
              "installed actor pipeline did not emit both events");
      require(result.emitted[0].type ==
                  "obcx::message_store::events::MessageStored",
              "message-store did not emit MessageStored");
      require(result.emitted[1].type == "bridge::events::MessageForwarded",
              "bridge did not emit MessageForwarded");
      require(result.emitted[1].payload.value("source_conversation_id", "") ==
                      "group:conformance" &&
                  result.emitted[1].payload.value("target_conversation_id",
                                                  "") == "chat:conformance",
              "bridge did not emit complete conversation identity");
      require(forwarder->seen_messages.size() == sequence,
              "bridge forwarder received an unexpected message count");
      require(forwarder->seen_messages.back().raw == input.raw,
              "raw payload was not preserved across the actor pipeline");
    }

    const auto persisted =
        scalar_count(*db_manager, "message_store_qq_messages");
    const auto forwarded = scalar_count(*db_manager, "bridge_message_mappings");
    require(persisted == static_cast<std::int64_t>(message_count),
            "message-store did not persist the pipeline message");
    require(forwarded == static_cast<std::int64_t>(message_count),
            "bridge did not persist the forwarding mapping");

    orchestrator.shutdown();
    blocking_executor->shutdown();
    work_guard.reset();
    io_context.stop();
    std::filesystem::remove(database);
    std::printf("messages=%llu persisted=%lld forwarded=%lld failures=0\n",
                static_cast<unsigned long long>(message_count),
                static_cast<long long>(persisted),
                static_cast<long long>(forwarded));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "actor pipeline conformance failed: %s\n",
                 error.what());
    std::filesystem::remove(database);
    return 2;
  }
}

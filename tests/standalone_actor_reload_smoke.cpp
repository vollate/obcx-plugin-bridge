#include "common/config_loader.hpp"
#include "core/actor_runtime_reload_controller.hpp"
#include "core/bot_operation_dispatcher.hpp"
#include "core/bot_registry.hpp"
#include "core/db_manager.hpp"
#include "core/message_event_ingress.hpp"
#include "core/orchestrator.hpp"
#include "core/qq_bot.hpp"
#include "core/qq_telegram_bot_endpoints.hpp"
#include "core/tg_bot.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "telegram/adapter/protocol_adapter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

class LiveQQBot final : public obcx::core::QQBot {
public:
  LiveQQBot() : QQBot(obcx::adapter::onebot11::ProtocolAdapter{}) {}

  void dispatch_message(obcx::common::MessageEvent event) {
    dispatcher_->dispatch(this, obcx::common::Event{std::move(event)});
  }

  void connect(const obcx::network::ConnectionManagerFactory::ConnectionType,
               const obcx::common::ConnectionConfig &) override {
    ++connect_calls;
    running.store(true, std::memory_order_release);
  }

  void run() override {
    ++run_calls;
    running.store(true, std::memory_order_release);
    io_context_->restart();
    event_work_.emplace(io_context_->get_executor());
    event_thread_ = std::jthread([context = io_context_] { context->run(); });
  }

  void stop() override {
    ++stop_calls;
    running.store(false, std::memory_order_release);
    event_work_.reset();
    io_context_->stop();
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
  }

  auto get_group_member_info(std::string_view, std::string_view user_id, bool)
      -> asio::awaitable<std::string> override {
    co_return "{\"status\":\"ok\",\"data\":{\"user_id\":\"" +
        std::string{user_id} +
        "\",\"nickname\":\"reload-user\",\"card\":\"\"}}";
  }

  std::atomic_int connect_calls = 0;
  std::atomic_int run_calls = 0;
  std::atomic_int stop_calls = 0;
  std::atomic_bool running = false;

private:
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>>
      event_work_;
  std::jthread event_thread_;
};

class RecordingTelegramBot final : public obcx::core::TGBot {
public:
  RecordingTelegramBot() : TGBot(obcx::adapter::telegram::ProtocolAdapter{}) {}

  void connect(const obcx::network::ConnectionManagerFactory::ConnectionType,
               const obcx::common::ConnectionConfig &) override {
    ++connect_calls;
    running.store(true, std::memory_order_release);
  }

  void run() override {
    ++run_calls;
    running.store(true, std::memory_order_release);
  }

  void stop() override {
    ++stop_calls;
    running.store(false, std::memory_order_release);
  }

  auto send_group_message(std::string_view group_id,
                          const obcx::common::Message &message)
      -> asio::awaitable<std::string> override {
    std::size_t sequence = 0;
    bool fail = false;
    {
      std::scoped_lock lock(mutex_);
      destinations_.emplace_back(group_id);
      std::string text;
      for (const auto &segment : message) {
        if (segment.type == "text") {
          text += segment.data.value("text", std::string{});
        }
      }
      messages_.push_back(std::move(text));
      sequence = destinations_.size();
      if (failures_remaining_ > 0) {
        --failures_remaining_;
        fail = true;
      }
    }
    if (fail) {
      co_return R"({"ok":false,"error_code":429,"description":"rate limited","parameters":{"retry_after":1}})";
    }
    co_return "{\"ok\":true,\"result\":{\"message_id\":" +
        std::to_string(1000 + sequence) + "}}";
  }

  void fail_next_group_send() {
    std::scoped_lock lock(mutex_);
    ++failures_remaining_;
  }

  [[nodiscard]] auto destinations() const -> std::vector<std::string> {
    std::scoped_lock lock(mutex_);
    return destinations_;
  }

  [[nodiscard]] auto messages() const -> std::vector<std::string> {
    std::scoped_lock lock(mutex_);
    return messages_;
  }

  std::atomic_int connect_calls = 0;
  std::atomic_int run_calls = 0;
  std::atomic_int stop_calls = 0;
  std::atomic_bool running = false;

private:
  mutable std::mutex mutex_;
  std::vector<std::string> destinations_;
  std::vector<std::string> messages_;
  std::size_t failures_remaining_ = 0;
};

class IngressProbe {
public:
  void record(std::string message_id, obcx::core::OrchestratorResult result) {
    std::scoped_lock lock(mutex_);
    results_.insert_or_assign(std::move(message_id), std::move(result));
  }

  [[nodiscard]] auto find(const std::string &message_id) const
      -> std::optional<obcx::core::OrchestratorResult> {
    std::scoped_lock lock(mutex_);
    const auto found = results_.find(message_id);
    return found == results_.end()
               ? std::nullopt
               : std::optional<obcx::core::OrchestratorResult>{found->second};
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, obcx::core::OrchestratorResult> results_;
};

void require(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void write_file(const fs::path &path, const std::string &contents) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("cannot write reload smoke configuration");
  }
  output << contents;
}

auto config_document(const fs::path &database, const fs::path &files,
                     std::string_view message_store, std::string_view bridge,
                     std::string_view telegram_group) -> std::string {
  return "[bots.qq_live]\n"
         "type = \"qq\"\n"
         "enabled = true\n\n"
         "[bots.qq_live.connection]\n"
         "type = \"websocket\"\n"
         "host = \"127.0.0.1\"\n"
         "port = 3001\n"
         "access_token = \"reload-smoke-token\"\n\n"
         "[bots.telegram_live]\n"
         "type = \"telegram\"\n"
         "enabled = true\n\n"
         "[bots.telegram_live.connection]\n"
         "type = \"http\"\n"
         "host = \"api.invalid\"\n"
         "port = 443\n"
         "access_token = \"reload-smoke-token\"\n\n"
         "[db.instances.main]\n"
         "type = \"sqlite\"\n"
         "path = \"" +
         database.string() +
         "\"\n\n"
         "[actor_runtime]\n"
         "workers = 2\n"
         "blocking_workers = 1\n\n"
         "[actors.message_store]\n"
         "library = \"" +
         std::string{message_store} +
         "\"\n"
         "enabled = true\n"
         "partition = \"source_platform:conversation_id\"\n"
         "db = \"main\"\n"
         "db_namespace = \"message_store\"\n\n"
         "[actors.bridge]\n"
         "library = \"" +
         std::string{bridge} +
         "\"\n"
         "enabled = true\n"
         "requires = [\"message_store\"]\n"
         "partition = \"source_platform:conversation_id\"\n"
         "db = \"main\"\n"
         "db_namespace = \"bridge\"\n\n"
         "[actors.bridge.config]\n"
         "telegram_installation = \"telegram_live\"\n"
         "onebot11_installation = \"qq_live\"\n"
         "enable_retry_queue = true\n"
         "message_retry_base_interval_sec = 300\n"
         "retry_queue_check_interval_sec = 300\n"
         "max_retry_interval_sec = 300\n"
         "bridge_files_dir = \"" +
         files.string() +
         "\"\n"
         "ffmpeg_path = \"ffmpeg\"\n\n"
         "[pipelines.message]\n"
         "source = \"obcx::core::events::RawMessageEvent\"\n\n"
         "[[pipelines.message.stages]]\n"
         "name = \"persist\"\n"
         "actor = \"message_store\"\n"
         "input = \"obcx::core::events::RawMessageEvent\"\n"
         "output = \"obcx::message_store::events::MessageStored\"\n"
         "mode = \"await\"\n\n"
         "[[pipelines.message.stages]]\n"
         "name = \"forward\"\n"
         "actor = \"bridge\"\n"
         "input = \"obcx::message_store::events::MessageStored\"\n"
         "output = [\"bridge::events::MessageForwarded\", "
         "\"bridge::events::MessageForwardFailed\"]\n"
         "after = [\"persist\"]\n"
         "mode = \"await\"\n\n"
         "[[group_mappings.group_to_group]]\n"
         "telegram_group_id = \"" +
         std::string{telegram_group} +
         "\"\n"
         "qq_group_id = \"qq-source-group\"\n"
         "show_qq_to_tg_sender = false\n"
         "show_tg_to_qq_sender = false\n"
         "enable_qq_to_tg = true\n"
         "enable_tg_to_qq = true\n";
}

auto smoke_text(const std::size_t sequence) -> std::string {
  return sequence == 4 ? "/tp 2072 ~ 1080"
                       : "reload smoke " + std::to_string(sequence);
}

auto raw_message(const std::size_t sequence) -> obcx::core::MessageEnvelope {
  const auto suffix = std::to_string(sequence);
  const auto text = smoke_text(sequence);
  obcx::core::MessageEnvelope message;
  message.id = "reload-root-" + suffix;
  message.type = "obcx::core::events::RawMessageEvent";
  message.source_platform = "qq";
  message.source_bot = "qq_live";
  message.conversation_id = "group:qq-source-group";
  message.correlation_id = "reload-correlation-" + suffix;
  message.payload = {
      {"message_id", "reload-message-" + suffix},
      {"conversation_id", message.conversation_id},
      {"sender", "reload-user"},
      {"group_id", "qq-source-group"},
      {"message_type", "group"},
      {"payload", {{"text", text}}},
  };
  message.raw = {
      {"post_type", "message"},
      {"message_type", "group"},
      {"sub_type", "normal"},
      {"message_id", "reload-message-" + suffix},
      {"user_id", "reload-user"},
      {"group_id", "qq-source-group"},
      {"raw_message", text},
      {"message", {{{"type", "text"}, {"data", {{"text", text}}}}}},
  };
  return message;
}

auto message_event(const std::size_t sequence) -> obcx::common::MessageEvent {
  const auto suffix = std::to_string(sequence);
  const auto text = smoke_text(sequence);
  obcx::common::MessageEvent event;
  event.type = obcx::common::EventType::message;
  event.time = std::chrono::system_clock::now();
  event.self_id = "qq-live-id";
  event.post_type = "message";
  event.data = obcx::common::json::object();
  event.message_type = "group";
  event.sub_type = "normal";
  event.message_id = "reload-message-" + suffix;
  event.user_id = "reload-user";
  event.message = {{.type = "text", .data = {{"text", text}}}};
  event.raw_message = text;
  event.group_id = "qq-source-group";
  return event;
}

auto process(
    asio::io_context &io,
    const std::shared_ptr<obcx::core::ActorRuntimeReloadController> &controller,
    const std::size_t sequence) -> obcx::core::OrchestratorResult {
  auto future = asio::co_spawn(io, controller->process(raw_message(sequence)),
                               asio::use_future);
  require(future.wait_for(20s) == std::future_status::ready,
          "bridge reload route timed out");
  return future.get();
}

auto start_reload(
    const std::shared_ptr<obcx::core::ActorRuntimeReloadController> &controller,
    obcx::core::RuntimeGenerationBuildRequest request)
    -> std::future<obcx::core::RuntimeReloadResult> {
  auto completion =
      std::make_shared<std::promise<obcx::core::RuntimeReloadResult>>();
  auto completed = completion->get_future();
  require(controller->start_reload(std::move(request), 2s,
                                   [completion](const auto &result) {
                                     completion->set_value(result);
                                   }) ==
              obcx::core::RuntimeReloadStartStatus::Accepted,
          "installed actor reload was not accepted");
  return completed;
}

auto retry_row_count(obcx::core::DbManager &database) -> std::int64_t {
  return database.run_read<std::int64_t>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            "SELECT COUNT(*) AS count FROM bridge_message_retry_queue;");
        return std::get<std::int64_t>(rows.front().at("count"));
      });
}

void make_retry_rows_ready(obcx::core::DbManager &database) {
  database.run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute(
        "UPDATE bridge_message_retry_queue SET next_retry_at = 0;");
  });
}

auto has_mapping(obcx::core::DbManager &database,
                 const std::string &source_message_id) -> bool {
  return database.run_read<bool>(
      "main", [&source_message_id](obcx::core::IDbConnection &connection) {
        return !connection
                    .query("SELECT target_message_id FROM "
                           "bridge_message_mappings WHERE source_platform = ? "
                           "AND source_message_id = ? AND target_platform = ? "
                           "LIMIT 1;",
                           {std::string{"qq"}, source_message_id,
                            std::string{"telegram"}})
                    .empty();
      });
}

} // namespace

auto main(int argc, char **argv) -> int {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s MESSAGE_STORE_ACTOR BRIDGE_ACTOR\n",
                 argv[0]);
    return 1;
  }

  const auto root =
      fs::temp_directory_path() /
      ("obcx-installed-actor-reload-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::shared_ptr<obcx::core::ActorRuntimeReloadController> controller;
  std::shared_ptr<LiveQQBot> qq;
  std::shared_ptr<RecordingTelegramBot> telegram;
  asio::io_context io;
  auto work_guard = asio::make_work_guard(io);
  std::jthread io_thread([&io] { io.run(); });

  try {
    fs::create_directories(root / "files");
    const auto config_path = root / "runtime.toml";
    const auto database_path = root / "runtime.sqlite3";
    write_file(config_path, config_document(database_path, root / "files",
                                            argv[1], argv[2], "telegram-old"));

    auto parsed = obcx::core::RuntimeGenerationBuilder::parse_config(
        config_path.string());
    require(static_cast<bool>(parsed), "initial reload config did not parse");

    auto database = obcx::core::DbManager::shared_manager(
        parsed.snapshot->get_db_instance_configs());
    auto registry = std::make_shared<obcx::core::BotRegistry>();
    qq = std::make_shared<LiveQQBot>();
    telegram = std::make_shared<RecordingTelegramBot>();
    obcx::common::ConnectionConfig connection;
    qq->connect(obcx::network::ConnectionManagerFactory::ConnectionType::
                    Onebot11WebSocket,
                connection);
    telegram->connect(
        obcx::network::ConnectionManagerFactory::ConnectionType::TelegramHTTP,
        connection);
    qq->run();
    telegram->run();
    registry->register_bot("qq", "qq_live", qq);
    registry->register_bot("telegram", "telegram_live", telegram);
    auto dispatcher =
        std::make_shared<obcx::core::QQTelegramOperationDispatcher>();
    obcx::core::register_existing_bot_operation_endpoint(*dispatcher, "qq_live",
                                                         "qq", qq);
    obcx::core::register_existing_bot_operation_endpoint(
        *dispatcher, "telegram_live", "telegram", telegram);

    obcx::core::RuntimeGenerationBuilder builder;
    auto initial = builder.build({
        .purpose = obcx::core::RuntimeGenerationBuildPurpose::Startup,
        .generation_id = 1,
        .snapshot = parsed.snapshot,
        .actor_search_directories = {fs::path{argv[1]}.parent_path(),
                                     fs::path{argv[2]}.parent_path()},
        .staging_root = root / "staging",
        .configured_io_sources = 1,
        .db_manager = database,
        .bot_registry = registry,
        .bot_operation_client = dispatcher,
        .require_registered_bots = true,
    });
    require(initial.ready(),
            initial.failure
                ? initial.failure->code + ": " + initial.failure->message
                : "initial actor generation was not ready");
    controller = std::make_shared<obcx::core::ActorRuntimeReloadController>(
        std::move(initial.generation));
    auto ingress_probe = std::make_shared<IngressProbe>();
    qq->on_event<obcx::common::MessageEvent>(
        [controller, ingress_probe](
            obcx::core::IBot &,
            const obcx::common::MessageEvent &event) -> asio::awaitable<void> {
          auto result = co_await controller->process(
              obcx::core::raw_message_envelope_from_event("qq", "qq_live",
                                                          event));
          ingress_probe->record(event.message_id, std::move(result));
          co_return;
        });

    const auto make_reload_request = [&] {
      return obcx::core::RuntimeGenerationBuildRequest{
          .config_path = config_path.string(),
          .actor_search_directories = {fs::path{argv[1]}.parent_path(),
                                       fs::path{argv[2]}.parent_path()},
          .staging_root = root / "staging",
          .configured_io_sources = 1,
      };
    };
    const auto finish_reload = [](auto &reload) {
      require(reload.wait_for(20s) == std::future_status::ready,
              "installed actor reload timed out");
      auto result = reload.get();
      require(result.succeeded(),
              result.failure
                  ? result.failure->code + ": " + result.failure->message
                  : "installed actor reload failed");
      return result;
    };

    for (std::size_t reload_index = 0; reload_index < 3; ++reload_index) {
      auto cold_reload = start_reload(controller, make_reload_request());
      (void)finish_reload(cold_reload);
    }
    require(controller->active_generation()->id() == 4,
            "cold reload sequence did not reach generation 4");

    telegram->fail_next_group_send();
    require(!process(io, controller, 1).ok(),
            "pre-reload send failure did not reach the retry queue");
    require(retry_row_count(*database) == 1,
            "pre-reload send failure was not persisted for retry");
    make_retry_rows_ready(*database);
    require(telegram->destinations() ==
                std::vector<std::string>{"telegram-old"},
            "pre-reload route did not use the old group mapping");

    write_file(config_path, config_document(database_path, root / "files",
                                            argv[1], argv[2], "telegram-new"));
    for (std::size_t reload_index = 0; reload_index < 2; ++reload_index) {
      auto quiet_reload = start_reload(controller, make_reload_request());
      (void)finish_reload(quiet_reload);
    }
    require(controller->active_generation()->id() == 6,
            "active reload sequence did not reach generation 6");

    auto held_route = controller->active_generation()->admit_route();
    require(static_cast<bool>(held_route),
            "active generation did not admit a held route");
    auto reloaded = start_reload(controller, make_reload_request());
    qq->dispatch_message(message_event(2));
    const auto gate_deadline = std::chrono::steady_clock::now() + 20s;
    while (controller->gate_open() &&
           std::chrono::steady_clock::now() < gate_deadline) {
      std::this_thread::sleep_for(1ms);
    }
    require(!controller->gate_open(),
            "installed actor reload gate did not close");
    qq->dispatch_message(message_event(3));
    held_route.reset();
    const auto reload_result = finish_reload(reloaded);
    require(reload_result.active_generation_id == 7,
            "message-bearing reload did not reach generation 7");

    auto invalid_retry_config = config_document(
        database_path, root / "files", argv[1], argv[2], "telegram-rejected");
    const auto retry_marker =
        invalid_retry_config.find("enable_retry_queue = true\n");
    require(retry_marker != std::string::npos,
            "retry configuration marker was not generated");
    invalid_retry_config.insert(
        retry_marker + std::string{"enable_retry_queue = true\n"}.size(),
        "message_retry_max_attempts = 0\n");
    write_file(config_path, invalid_retry_config);
    auto invalid_reload = asio::co_spawn(
        io,
        controller->reload(
            obcx::core::RuntimeGenerationBuildRequest{
                .config_path = config_path.string(),
                .actor_search_directories = {fs::path{argv[1]}.parent_path(),
                                             fs::path{argv[2]}.parent_path()},
                .staging_root = root / "staging",
                .configured_io_sources = 1,
            },
            2s),
        asio::use_future);
    require(invalid_reload.wait_for(20s) == std::future_status::ready,
            "invalid bridge retry reload timed out");
    const auto invalid_reload_result = invalid_reload.get();
    require(!invalid_reload_result.succeeded() &&
                invalid_reload_result.failure &&
                invalid_reload_result.failure->code ==
                    "reload_actor_config_invalid",
            "invalid bridge retry configuration was not rejected");
    require(controller->active_generation()->id() ==
                reload_result.active_generation_id,
            "invalid bridge retry reload replaced the active generation");

    qq->dispatch_message(message_event(4));
    const auto route_deadline = std::chrono::steady_clock::now() + 20s;
    for (std::size_t sequence = 2; sequence <= 4; ++sequence) {
      const auto message_id = "reload-message-" + std::to_string(sequence);
      auto ingress_result = ingress_probe->find(message_id);
      while (!ingress_result &&
             std::chrono::steady_clock::now() < route_deadline) {
        std::this_thread::sleep_for(10ms);
        ingress_result = ingress_probe->find(message_id);
      }
      require(ingress_result.has_value(),
              "EventDispatcher ingress did not finish for sequence " +
                  std::to_string(sequence));
      if (!ingress_result->ok()) {
        const auto &failure = ingress_result->failures.front().failure;
        require(false, "EventDispatcher ingress failed for sequence " +
                           std::to_string(sequence) + " with " + failure.code +
                           ": " + failure.message);
      }
      while (!has_mapping(*database, message_id) &&
             std::chrono::steady_clock::now() < route_deadline) {
        std::this_thread::sleep_for(10ms);
      }
      require(has_mapping(*database, message_id),
              "post-reload EventDispatcher route did not finish for sequence " +
                  std::to_string(sequence));
    }
    const auto retry_deadline = std::chrono::steady_clock::now() + 20s;
    while (!has_mapping(*database, "reload-message-1") &&
           std::chrono::steady_clock::now() < retry_deadline) {
      std::this_thread::sleep_for(10ms);
    }
    require(has_mapping(*database, "reload-message-1") &&
                retry_row_count(*database) == 0,
            "candidate generation did not restore and finish the pending "
            "retry");
    const auto destinations = telegram->destinations();
    require(std::ranges::count(destinations, std::string{"telegram-old"}) ==
                    2 &&
                std::ranges::count(destinations, std::string{"telegram-new"}) ==
                    3 &&
                std::ranges::count(destinations,
                                   std::string{"telegram-rejected"}) == 0,
            "reload retry ownership or post-cutover routing was incorrect");
    require(controller->active_generation()->bot_registry() == registry,
            "reload replaced the process-owned BotRegistry");
    require(registry->find_bot("qq", "qq_live")->bot == qq &&
                registry->find_bot("telegram", "telegram_live")->bot ==
                    telegram,
            "reload replaced live bot instances");
    require(qq->running.load(std::memory_order_acquire) &&
                telegram->running.load(std::memory_order_acquire),
            "reload stopped a live bot connection");
    require(qq->connect_calls == 1 && telegram->connect_calls == 1 &&
                qq->run_calls == 1 && telegram->run_calls == 1 &&
                qq->stop_calls == 0 && telegram->stop_calls == 0,
            "reload reconnected or stopped a live bot");
    require(std::ranges::count(telegram->messages(),
                               std::string{"/tp 2072 ~ 1080"}) == 1,
            "unmatched slash message did not follow the ordinary Bridge "
            "pipeline exactly once");

    controller->shutdown();
    controller.reset();
    qq->stop();
    telegram->stop();
    work_guard.reset();
    io.stop();
    fs::remove_all(root);
    std::printf("reload=ok old_routes=1 new_routes=3 bot_reconnects=0\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "installed actor reload smoke failed: %s\n",
                 error.what());
    if (controller) {
      controller->shutdown();
      controller.reset();
    }
    if (qq && qq->running.load(std::memory_order_acquire)) {
      qq->stop();
    }
    if (telegram && telegram->running.load(std::memory_order_acquire)) {
      telegram->stop();
    }
    work_guard.reset();
    io.stop();
    fs::remove_all(root);
    return 2;
  }
}

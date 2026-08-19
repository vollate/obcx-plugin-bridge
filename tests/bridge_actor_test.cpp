#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
#include "core/blocking_executor.hpp"
#include "core/bot_registry.hpp"
#include "core/db_manager.hpp"
#include "core/native_actor_scheduler.hpp"
#include "qq/message_formatter.hpp"
#include "qq/photo_normalizer.hpp"
#include "telegram/handler.hpp"
#if __has_include("core/bot_operation_dispatcher.hpp") &&                      \
                  __has_include("core/qq_telegram_bot_endpoints.hpp")
#if __has_include("core/qq_bot.hpp") && __has_include("core/tg_bot.hpp")
#define OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS 1
#include "core/bot_operation_dispatcher.hpp"
#include "core/qq_bot.hpp"
#include "core/qq_telegram_bot_endpoints.hpp"
#include "core/tg_bot.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "telegram/adapter/protocol_adapter.hpp"
#else
#define OBCX_BRIDGE_HAS_CONCRETE_BOT_TESTS 0
#endif
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
    config << "[bots.qq-main]\n"
              "type = \"qq\"\n"
              "enabled = true\n\n"
              "[bots.tg-main]\n"
              "type = \"telegram\"\n"
              "enabled = true\n\n"
              "[actors.bridge.config]\n"
              "telegram_installation = \"tg-main\"\n"
              "onebot11_installation = \"qq-main\"\n"
              "bridge_files_dir = \"/tmp/bridge_files\"\n"
              "enable_retry_queue = "
           << (enable_retry ? "true\n" : "false\n")
           << "message_retry_max_attempts = 5\n"
              "message_retry_base_interval_sec = "
           << retry_interval_seconds
           << "\nretry_queue_check_interval_sec = " << retry_interval_seconds
           << "\nmax_retry_interval_sec = "
           << std::max(2, retry_interval_seconds * 2)
           << "\nimage_url_probe_max_attempts = 1\n"
              "image_url_probe_base_delay_ms = 1\n"
              "image_url_probe_timeout_ms = 1\n\n";
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

  auto send_group_message(std::string_view group_id,
                          const obcx::common::Message &message)
      -> asio::awaitable<std::string> override {
    last_group_id = group_id;
    last_message = message;
    const auto call = send_group_calls.fetch_add(1);
    if (force_malformed_send.load(std::memory_order_acquire)) {
      co_return "{}";
    }
    if (always_succeed.load(std::memory_order_acquire) ||
        (succeed_after_first.load(std::memory_order_acquire) && call > 0)) {
      co_return "{\"status\":\"ok\",\"data\":{\"message_id\":9001}}";
    }
    co_return R"({"status":"failed","retcode":-1,"message":"temporary unavailable","data":null})";
  }

  auto get_group_member_info(std::string_view, std::string_view user_id, bool)
      -> asio::awaitable<std::string> override {
    co_return "{\"status\":\"ok\",\"data\":{\"user_id\":\"" +
        std::string{user_id} + "\",\"nickname\":\"retry-user\",\"card\":\"\"}}";
  }

  auto delete_message(std::string_view)
      -> asio::awaitable<std::string> override {
    delete_message_calls.fetch_add(1, std::memory_order_relaxed);
    co_return R"({"status":"ok","retcode":0,"data":null})";
  }

  auto get_forward_msg(std::string_view)
      -> asio::awaitable<std::string> override {
    co_return forward_response;
  }

  std::atomic_int send_group_calls = 0;
  std::atomic_int delete_message_calls = 0;
  std::string last_group_id;
  obcx::common::Message last_message;
  std::atomic_bool always_succeed = false;
  std::atomic_bool succeed_after_first = false;
  std::atomic_bool force_malformed_send = false;
  std::string forward_response = "{}";
};

class RetryTestTelegramBot final : public obcx::core::TGBot {
public:
  RetryTestTelegramBot() : TGBot(obcx::adapter::telegram::ProtocolAdapter{}) {}

  auto send_group_message(std::string_view, const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    const auto call = send_group_calls.fetch_add(1);
    if (force_malformed_send.load(std::memory_order_acquire)) {
      co_return "{}";
    }
    if (always_succeed.load(std::memory_order_acquire) ||
        (succeed_after_first.load(std::memory_order_acquire) && call > 0)) {
      co_return "{\"ok\":true,\"result\":{\"message_id\":8001}}";
    }
    co_return R"({"ok":false,"error_code":429,"description":"rate limited","parameters":{"retry_after":1}})";
  }

  auto send_topic_message(std::string_view, int64_t,
                          const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    const auto call = send_topic_calls.fetch_add(1);
    if (always_succeed.load(std::memory_order_acquire) ||
        (succeed_after_first.load(std::memory_order_acquire) && call > 0)) {
      co_return "{\"ok\":true,\"result\":{\"message_id\":8002}}";
    }
    co_return R"({"ok":false,"error_code":429,"description":"rate limited","parameters":{"retry_after":1}})";
  }

  auto get_media_download_url(const obcx::core::MediaFileInfo &file)
      -> asio::awaitable<std::optional<std::string>> override {
    last_fetched_file = file;
    media_resolve_calls.fetch_add(1, std::memory_order_relaxed);
    co_return media_download_url;
  }

  auto download_file_content(std::string_view url)
      -> asio::awaitable<std::string> override {
    last_download_url = url;
    media_download_calls.fetch_add(1, std::memory_order_relaxed);
    co_return media_file_content;
  }

  auto send_media_group(
      std::string_view,
      const std::vector<std::pair<std::string, std::string>> &media,
      std::string_view, std::optional<int64_t>, std::optional<std::string>)
      -> asio::awaitable<std::string> override {
    send_media_group_calls.fetch_add(1, std::memory_order_relaxed);
    media_group_sizes.push_back(media.size());
    co_return R"({"ok":true,"result":[{"message_id":8101},{"message_id":8102}]})";
  }

  auto send_media_group_with_entities(
      std::string_view chat_id,
      const std::vector<std::pair<std::string, std::string>> &media,
      std::string_view caption, std::optional<int64_t> topic_id,
      std::optional<std::string> reply_to_message_id,
      const std::vector<obcx::core::TelegramTextEntity> &caption_entities)
      -> asio::awaitable<std::string> override {
    last_media_group_entities = caption_entities;
    co_return co_await send_media_group(chat_id, media, caption, topic_id,
                                        std::move(reply_to_message_id));
  }

  std::atomic_int send_group_calls = 0;
  std::atomic_int send_topic_calls = 0;
  std::atomic_int send_media_group_calls = 0;
  std::atomic_int media_resolve_calls = 0;
  std::atomic_int media_download_calls = 0;
  std::vector<size_t> media_group_sizes;
  std::optional<std::string> media_download_url =
      "https://api.telegram.test/file/bot123:secret/file.bin";
  std::string media_file_content = "telegram-media";
  std::string last_download_url;
  obcx::core::MediaFileInfo last_fetched_file;
  std::vector<obcx::core::TelegramTextEntity> last_media_group_entities;
  std::atomic_bool always_succeed = false;
  std::atomic_bool succeed_after_first = false;
  std::atomic_bool force_malformed_send = false;
};

class MultipartFallbackTelegramBot final : public obcx::core::TGBot {
public:
  MultipartFallbackTelegramBot()
      : TGBot(obcx::adapter::telegram::ProtocolAdapter{}) {}

  auto send_media_group(
      std::string_view,
      const std::vector<std::pair<std::string, std::string>> &,
      std::string_view, std::optional<int64_t>, std::optional<std::string>)
      -> asio::awaitable<std::string> override {
    url_send_calls.fetch_add(1, std::memory_order_relaxed);
    co_return R"({"ok":false,"error_code":400,"description":"failed to get HTTP URL content"})";
  }

  auto send_media_group_with_entities(
      std::string_view chat_id,
      const std::vector<std::pair<std::string, std::string>> &media,
      std::string_view caption, std::optional<int64_t> topic_id,
      std::optional<std::string> reply_to_message_id,
      const std::vector<obcx::core::TelegramTextEntity> &)
      -> asio::awaitable<std::string> override {
    co_return co_await send_media_group(chat_id, media, caption, topic_id,
                                        std::move(reply_to_message_id));
  }

  auto send_media_group_uploads(
      std::string_view,
      const std::vector<obcx::core::TelegramMediaUpload> &media,
      std::string_view caption, std::optional<int64_t>,
      std::optional<std::string>) -> asio::awaitable<std::string> override {
    multipart_send_calls.fetch_add(1, std::memory_order_relaxed);
    uploaded_media = media;
    last_caption = caption;
    for (const auto &entry : std::filesystem::directory_iterator(
             std::filesystem::temp_directory_path())) {
      if (!entry.is_directory() ||
          !entry.path().filename().string().starts_with("obcx-qq-media-")) {
        continue;
      }
      const auto first_file = entry.path() / "qq-media-0.jpg";
      std::ifstream input(first_file, std::ios::binary);
      const std::string data{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
      if (data == "download-one") {
        observed_temporary_root = entry.path();
        break;
      }
    }
    if (multipart_fails.load(std::memory_order_acquire)) {
      throw std::runtime_error(multipart_error);
    }
    co_return R"({"ok":true,"result":[{"message_id":8201},{"message_id":8202}]})";
  }

  auto send_media_group_uploads_with_entities(
      std::string_view chat_id,
      const std::vector<obcx::core::TelegramMediaUpload> &media,
      std::string_view caption, std::optional<int64_t> topic_id,
      std::optional<std::string> reply_to_message_id,
      const std::vector<obcx::core::TelegramTextEntity> &caption_entities)
      -> asio::awaitable<std::string> override {
    last_caption_entities = caption_entities;
    co_return co_await send_media_group_uploads(
        chat_id, media, caption, topic_id, std::move(reply_to_message_id));
  }

  std::atomic_int url_send_calls = 0;
  std::atomic_int multipart_send_calls = 0;
  std::atomic_bool multipart_fails = false;
  std::vector<obcx::core::TelegramMediaUpload> uploaded_media;
  std::filesystem::path observed_temporary_root;
  std::string last_caption;
  std::vector<obcx::core::TelegramTextEntity> last_caption_entities;
  std::string multipart_error = "multipart upload failed";
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

auto bridge_operations(obcx::core::IBot &qq_bot, obcx::core::IBot &telegram_bot)
    -> std::shared_ptr<bridge::BridgeBotOperations> {
  auto qq_alias =
      std::shared_ptr<obcx::core::IBot>(&qq_bot, [](obcx::core::IBot *) {});
  auto telegram_alias = std::shared_ptr<obcx::core::IBot>(
      &telegram_bot, [](obcx::core::IBot *) {});
  auto dispatcher =
      std::make_shared<obcx::core::QQTelegramOperationDispatcher>();
  obcx::core::register_existing_bot_operation_endpoint(*dispatcher, "qq-main",
                                                       "qq", qq_alias);
  obcx::core::register_existing_bot_operation_endpoint(
      *dispatcher, "tg-main", "telegram", telegram_alias);
  return std::make_shared<bridge::BridgeBotOperations>(dispatcher, "tg-main",
                                                       "qq-main");
}

auto bridge_operations_for_telegram(obcx::core::IBot &telegram_bot)
    -> std::shared_ptr<bridge::BridgeBotOperations> {
  static RetryTestQQBot onebot_stub;
  return bridge_operations(onebot_stub, telegram_bot);
}

auto bridge_operations_for_onebot(obcx::core::IBot &qq_bot)
    -> std::shared_ptr<bridge::BridgeBotOperations> {
  static RetryTestTelegramBot telegram_stub;
  telegram_stub.always_succeed.store(true, std::memory_order_release);
  return bridge_operations(qq_bot, telegram_stub);
}

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
  auto dispatcher =
      std::make_shared<obcx::core::QQTelegramOperationDispatcher>();
  obcx::core::register_existing_bot_operation_endpoint(*dispatcher, "qq-main",
                                                       "qq", qq_bot);
  obcx::core::register_existing_bot_operation_endpoint(
      *dispatcher, "tg-main", "telegram", telegram_bot);

  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<obcx::core::BotRegistry>(registry);
  services->register_service<obcx::bot::BotOperationClient>(dispatcher);
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
[bots.qq-main]
type = "qq"
enabled = true

[bots.tg-main]
type = "telegram"
enabled = true

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
        .disposition = bridge::DirectForwardDisposition::NewDelivery,
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
  ASSERT_TRUE(contract.contains("configuration"));
  EXPECT_EQ(contract["configuration"]["required_strings"],
            obcx::common::json::array({"bridge_files_dir"}));
  EXPECT_EQ(
      contract["configuration"]["bot_installations"]["telegram_installation"],
      "telegram");
  EXPECT_EQ(
      contract["configuration"]["bot_installations"]["onebot11_installation"],
      "qq");
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
      repository
          ->get_target_message_id("qq", "qq-persist-failure-1", "telegram")
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
  auto dispatcher =
      std::make_shared<obcx::core::QQTelegramOperationDispatcher>();
  obcx::core::register_existing_bot_operation_endpoint(*dispatcher, "qq-main",
                                                       "qq", qq_bot);
  obcx::core::register_existing_bot_operation_endpoint(
      *dispatcher, "tg-main", "telegram", telegram_bot);
  auto services = std::make_shared<obcx::core::ActorServices>();
  services->register_service<obcx::core::DbManager>(db_manager);
  services->register_service<obcx::core::BotRegistry>(registry);
  services->register_service<obcx::bot::BotOperationClient>(dispatcher);
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

TEST(BridgeActorTest, QqSenderPrefixStylesWholeBracketedLabelForTelegram) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  RetryTestQQBot qq_bot;
  bridge::qq::QQMessageFormatter formatter(bridge_operations_for_onebot(qq_bot),
                                           config);
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", true,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.user_id = "张😀";
  event.group_id = "qq-group";
  obcx::common::Message output;

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.format_sender_info(event, &bridge_config, "qq-group",
                                   "tg-group", -1, output),
      asio::use_future);
  ioc.run();

  EXPECT_EQ(future.get(), "张😀");
  ASSERT_EQ(output.size(), 2U);
  EXPECT_EQ(output[0].type, "text");
  EXPECT_EQ(output[0].data.value("text", ""), "[张😀]");
  EXPECT_EQ(output[0].data.value("telegram_style", ""), "italic");
  EXPECT_EQ(output[1].data.value("text", ""), "\t");
  EXPECT_FALSE(output[1].data.contains("telegram_style"));
}

TEST(BridgeActorTest, RejectsMissingOrMismatchedSourceBotBeforeProviderIo) {
  const auto db_path = temp_db_path("source_bot_rejection");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  telegram_bot->always_succeed.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);

  auto missing = bridge_message_stored("qq", "qq-missing-source", "qq-group");
  missing.source_bot.clear();
  const auto missing_result = run_actor(services, std::move(missing));
  EXPECT_FALSE(missing_result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);

  auto mismatched = bridge_message_stored("qq", "qq-wrong-source", "qq-group");
  mismatched.source_bot = "qq-other";
  const auto mismatched_result = run_actor(services, std::move(mismatched));
  EXPECT_FALSE(mismatched_result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);

  auto notice = raw_poke_notice();
  notice.source_bot.clear();
  const auto notice_result = run_actor(services, std::move(notice));
  EXPECT_FALSE(notice_result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, QqDirectForwardUsesOneDedupReadAndOneActorWrite) {
  const auto db_path = temp_db_path("qq_direct_operation_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();

  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  telegram_bot->always_succeed.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor(
      services, bridge_message_stored("qq", "qq-direct-1", "qq-group"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "8001");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 1);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.pre_send_deduplication_reads, 1U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(counts.direct_forward_writes, 1U);
  EXPECT_EQ(counts.retry_completion_writes, 0U);
  EXPECT_EQ(counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(repository->get_target_message_id("qq", "qq-direct-1", "telegram"),
            "8001");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramDirectForwardUsesOneDedupReadAndOneActorWrite) {
  const auto db_path = temp_db_path("telegram_direct_operation_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();

  auto qq_bot = std::make_shared<RetryTestQQBot>();
  qq_bot->always_succeed.store(true, std::memory_order_release);
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor(
      services, bridge_message_stored("telegram", "tg-direct-1", "tg-group"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "9001");
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.pre_send_deduplication_reads, 1U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(counts.direct_forward_writes, 1U);
  EXPECT_EQ(counts.retry_completion_writes, 0U);
  EXPECT_EQ(counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(repository->get_target_message_id("telegram", "tg-direct-1", "qq"),
            "9001");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramMediaFetchIsBoundedAndLeavesNoTokenUrl) {
  const auto db_path = temp_db_path("telegram_media_fetch");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  qq_bot->always_succeed.store(true, std::memory_order_release);
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);

  auto stored = bridge_message_stored("telegram", "tg-media-1", "tg-group");
  stored.raw["raw_message"] = "[image]";
  stored.raw["message"] = nlohmann::json::array(
      {{{"type", "image"}, {"data", {{"file_id", "tg-file-1"}}}}});
  stored.raw["image"] = {{"file_id", "tg-file-1"},
                         {"file_unique_id", "unique-1"},
                         {"file_size", 14},
                         {"mime_type", "image/jpeg"},
                         {"file_name", "image.jpg"}};

  const auto result = run_actor(services, std::move(stored));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(telegram_bot->media_resolve_calls.load(), 1);
  EXPECT_EQ(telegram_bot->media_download_calls.load(), 1);
  EXPECT_EQ(telegram_bot->last_fetched_file.file_id, "tg-file-1");
  ASSERT_EQ(qq_bot->last_message.size(), 1U);
  EXPECT_EQ(qq_bot->last_message.front().type, "image");
  const auto file =
      qq_bot->last_message.front().data.value("file", std::string{});
  EXPECT_TRUE(file.starts_with("file:///"));
  EXPECT_EQ(file.find("bot123:secret"), std::string::npos);
  EXPECT_EQ(qq_bot->last_message.front().data.dump().find("api.telegram"),
            std::string::npos);

  const auto container_prefix =
      std::string{"file:///root/llonebot/bridge_files"};
  if (file.starts_with(container_prefix)) {
    const auto host_path =
        std::string{"/tmp/bridge_files"} + file.substr(container_prefix.size());
    EXPECT_FALSE(std::filesystem::exists(host_path));
  }

  telegram_bot->media_file_content = std::string(10U * 1024U * 1024U + 1U, 'x');
  auto oversized = bridge_message_stored("telegram", "tg-media-2", "tg-group");
  oversized.raw["raw_message"] = "[image]";
  oversized.raw["message"] = nlohmann::json::array(
      {{{"type", "image"}, {"data", {{"file_id", "tg-file-2"}}}}});
  const auto oversized_result = run_actor(services, std::move(oversized));
  EXPECT_FALSE(oversized_result.ok());
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramEditResendReturnsNewMappingForOneActorWrite) {
  const auto db_path = temp_db_path("telegram_edit_operation_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping(storage::MessageMapping{
      .source_platform = "telegram",
      .source_message_id = "tg-edit-1",
      .target_platform = "qq",
      .target_message_id = "qq-old-1",
      .created_at = std::chrono::system_clock::now(),
  }));
  repository->reset_message_mapping_operation_counts();

  auto qq_bot = std::make_shared<RetryTestQQBot>();
  qq_bot->always_succeed.store(true, std::memory_order_release);
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);
  services->register_service<bridge::BridgeStateRepository>(repository);
  auto stored = bridge_message_stored("telegram", "tg-edit-1", "tg-group");
  stored.raw["sub_type"] = "edited";

  const auto result = run_actor(services, std::move(stored));

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(qq_bot->delete_message_calls.load(), 1);
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.pre_send_deduplication_reads, 0U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(counts.direct_forward_writes, 1U);
  EXPECT_EQ(repository->get_target_message_id("telegram", "tg-edit-1", "qq"),
            "9001");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, AlreadyPersistedDirectForwardSkipsSendAndActorWrite) {
  const auto db_path = temp_db_path("already_persisted_operation_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping(storage::MessageMapping{
      .source_platform = "qq",
      .source_message_id = "qq-already-1",
      .target_platform = "telegram",
      .target_message_id = "tg-existing-1",
      .created_at = std::chrono::system_clock::now(),
  }));
  repository->reset_message_mapping_operation_counts();

  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  telegram_bot->always_succeed.store(true, std::memory_order_release);
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor(
      services, bridge_message_stored("qq", "qq-already-1", "qq-group"));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"],
            "tg-existing-1");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.pre_send_deduplication_reads, 1U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(counts.direct_forward_writes, 0U);
  EXPECT_EQ(counts.retry_completion_writes, 0U);
  EXPECT_EQ(counts.deferred_media_group_writes, 0U);

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, InlineQqMediaGroupReturnsPrimaryIdForOneActorWrite) {
  const auto db_path = temp_db_path("inline_qq_media_group_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();

  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto services =
      bridge_retry_services(db_manager, qq_bot, telegram_bot, false);
  services->register_service<bridge::BridgeStateRepository>(repository);
  auto stored = bridge_message_stored("qq", "qq-album-inline-1", "qq-group");
  stored.raw["raw_message"] = "[two images]";
  stored.raw["message"] = {
      {{"type", "image"}, {"data", {{"url", "invalid://image-one"}}}},
      {{"type", "image"}, {"data", {{"url", "invalid://image-two"}}}},
  };

  const auto result = run_actor(services, std::move(stored));

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.emitted.size(), 1U);
  EXPECT_EQ(result.emitted.front().type, "bridge::events::MessageForwarded");
  EXPECT_EQ(result.emitted.front().payload["target_message_id"], "8101");
  EXPECT_EQ(telegram_bot->send_media_group_calls.load(), 1);
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.pre_send_deduplication_reads, 1U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(counts.direct_forward_writes, 1U);
  EXPECT_EQ(counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(
      repository->get_target_message_id("qq", "qq-album-inline-1", "telegram"),
      "8101");

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest,
     QqMediaGroupBadRequestDownloadsMultipartUploadsAndDeletesTemporaryFiles) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  std::atomic_int download_calls = 0;
  MultipartFallbackTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor,
      [&download_calls](
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        download_calls.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(media.size(), 2U);
        co_return std::vector<bridge::qq::MediaDownloadResult>{
            {.image =
                 bridge::qq::DownloadedImage{.type = "photo",
                                             .original_url = media[0].second,
                                             .filename = "qq-media-0.jpg",
                                             .mime_type = "image/jpeg",
                                             .data = "download-one"}},
            {.image =
                 bridge::qq::DownloadedImage{.type = "photo",
                                             .original_url = media[1].second,
                                             .filename = "qq-media-1.png",
                                             .mime_type = "image/png",
                                             .data = "download-two"}},
        };
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      preserve_test_images);
  const std::vector<obcx::common::MessageSegment> image_segments = {
      {.type = "image", .data = {{"url", "invalid://one"}}},
      {.type = "image", .data = {{"url", "invalid://two"}}},
  };
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", true,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-multipart-fallback";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();

  EXPECT_TRUE(result.sent);
  EXPECT_EQ(result.primary_target_message_id, "8201");
  EXPECT_EQ(telegram_bot.url_send_calls.load(), 1);
  EXPECT_EQ(telegram_bot.multipart_send_calls.load(), 1);
  EXPECT_EQ(download_calls.load(), 1);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 2U);
  EXPECT_EQ(telegram_bot.last_caption, "[sender]");
  ASSERT_EQ(telegram_bot.last_caption_entities.size(), 1U);
  EXPECT_EQ(telegram_bot.last_caption_entities[0].type, "italic");
  EXPECT_EQ(telegram_bot.last_caption_entities[0].offset, 0U);
  EXPECT_EQ(telegram_bot.last_caption_entities[0].length, 8U);
  EXPECT_EQ(telegram_bot.uploaded_media[0].data, "download-one");
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "download-two");
  ASSERT_FALSE(telegram_bot.observed_temporary_root.empty());
  EXPECT_FALSE(std::filesystem::exists(telegram_bot.observed_temporary_root));

  telegram_bot.multipart_fails.store(true, std::memory_order_release);
  telegram_bot.observed_temporary_root.clear();
  asio::io_context failing_ioc;
  auto failing_future = asio::co_spawn(
      failing_ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  failing_ioc.run();
  const auto failing_result = failing_future.get();
  blocking_executor->shutdown();

  EXPECT_FALSE(failing_result.sent);
  EXPECT_EQ(telegram_bot.url_send_calls.load(), 2);
  EXPECT_EQ(telegram_bot.multipart_send_calls.load(), 2);
  EXPECT_EQ(download_calls.load(), 2);
  ASSERT_FALSE(telegram_bot.observed_temporary_root.empty());
  EXPECT_FALSE(std::filesystem::exists(telegram_bot.observed_temporary_root));
}

TEST(BridgeActorTest,
     QqMultipartFallbackNormalizesOverlongPhotoWithoutChangingPeers) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  MultipartFallbackTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor,
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        std::vector<bridge::qq::MediaDownloadResult> results;
        for (std::size_t index = 0; index < media.size(); ++index) {
          results.push_back(
              {.image = bridge::qq::DownloadedImage{
                   .type = "photo",
                   .original_url = media[index].second,
                   .filename = fmt::format("source-{}.jpg", index),
                   .mime_type = "image/jpeg",
                   .data = fmt::format("source-{}", index)}});
        }
        co_return results;
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      [](const bridge::BridgeConfig &,
         std::vector<bridge::qq::DownloadedImage> images) {
        EXPECT_EQ(images.size(), 3U);
        images[0].data = "normalized-overlong";
        return std::vector<bridge::qq::PhotoNormalizationResult>{
            {.image = std::move(images[0]),
             .normalized = true,
             .source_dimensions = {2048, 13301},
             .output_dimensions = {1332, 8657}},
            {.image = std::move(images[1]),
             .source_dimensions = {1024, 372},
             .output_dimensions = {1024, 372}},
            {.image = std::move(images[2]),
             .source_dimensions = {960, 960},
             .output_dimensions = {960, 960}},
        };
      });
  const std::vector<obcx::common::MessageSegment> image_segments = {
      {.type = "image", .data = {{"url", "invalid://overlong"}}},
      {.type = "image", .data = {{"url", "invalid://wide"}}},
      {.type = "image", .data = {{"url", "invalid://square"}}},
  };
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", false,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-dimension-normalization";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();
  blocking_executor->shutdown();

  EXPECT_TRUE(result.sent);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 3U);
  EXPECT_EQ(telegram_bot.uploaded_media[0].data, "normalized-overlong");
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "source-1");
  EXPECT_EQ(telegram_bot.uploaded_media[2].data, "source-2");
  EXPECT_NE(telegram_bot.last_caption.find("1 张图片尺寸已自动调整"),
            std::string::npos);
  EXPECT_EQ(telegram_bot.last_caption.find("占位图替换"), std::string::npos);
}

TEST(BridgeActorTest,
     QqForwardDimensionFailuresReplaceOnlyFailedItemsAndKeepCountsSeparate) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  std::atomic_int placeholder_downloads = 0;
  MultipartFallbackTelegramBot telegram_bot;
  RetryTestQQBot qq_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations(qq_bot, telegram_bot), config, nullptr,
      blocking_executor,
      [&placeholder_downloads](
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        if (media.size() == 1) {
          placeholder_downloads.fetch_add(1, std::memory_order_relaxed);
          co_return std::vector<bridge::qq::MediaDownloadResult>{
              {.image = bridge::qq::DownloadedImage{
                   .type = "photo",
                   .filename = "placeholder.jpg",
                   .mime_type = "image/jpeg",
                   .data = "dimension-placeholder"}}};
        }
        std::vector<bridge::qq::MediaDownloadResult> results;
        for (std::size_t index = 0; index < media.size(); ++index) {
          results.push_back(
              {.image = bridge::qq::DownloadedImage{
                   .type = "photo",
                   .original_url = media[index].second,
                   .filename = fmt::format("forward-{}.jpg", index),
                   .mime_type = "image/jpeg",
                   .data = fmt::format("forward-source-{}", index)}});
        }
        co_return results;
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      [](const bridge::BridgeConfig &,
         std::vector<bridge::qq::DownloadedImage> images) {
        if (images.size() == 1) {
          return preserve_test_images({}, std::move(images));
        }
        EXPECT_EQ(images.size(), 5U);
        images[0].data = "normalized-forward";
        return std::vector<bridge::qq::PhotoNormalizationResult>{
            {.image = std::move(images[0]),
             .normalized = true,
             .source_dimensions = {2048, 13301},
             .output_dimensions = {1332, 8657}},
            {.failure =
                 bridge::qq::PhotoNormalizationFailure::InvalidDimensions},
            {.failure =
                 bridge::qq::PhotoNormalizationFailure::UnsafeDimensions},
            {.failure =
                 bridge::qq::PhotoNormalizationFailure::NormalizationFailed},
            {.image = std::move(images[4]),
             .source_dimensions = {960, 960},
             .output_dimensions = {960, 960}},
        };
      });
  nlohmann::json messages = nlohmann::json::array();
  for (std::size_t index = 0; index < 5; ++index) {
    messages.push_back(
        {{"sender", {{"nickname", "node"}}},
         {"content",
          nlohmann::json::array(
              {{{"type", "image"},
                {"data", {{"url", fmt::format("invalid://{}", index)}}}}})}});
  }
  qq_bot.forward_response =
      nlohmann::json{{"status", "ok"}, {"data", {{"messages", messages}}}}
          .dump();
  obcx::common::Message output;
  const obcx::common::MessageSegment forward_segment{
      .type = "forward", .data = {{"id", "dimension-forward"}}};

  asio::io_context ioc;
  auto future = asio::co_spawn(ioc,
                               formatter.process_forward_message(
                                   forward_segment, "tg-group", -1, output),
                               asio::use_future);
  ioc.run();
  future.get();
  blocking_executor->shutdown();

  EXPECT_EQ(placeholder_downloads.load(), 1);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 5U);
  EXPECT_EQ(telegram_bot.uploaded_media[0].data, "normalized-forward");
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "dimension-placeholder");
  EXPECT_EQ(telegram_bot.uploaded_media[2].data, "dimension-placeholder");
  EXPECT_EQ(telegram_bot.uploaded_media[3].data, "dimension-placeholder");
  EXPECT_EQ(telegram_bot.uploaded_media[4].data, "forward-source-4");
  EXPECT_NE(telegram_bot.last_caption.find("1 张图片尺寸已自动调整"),
            std::string::npos);
  EXPECT_NE(telegram_bot.last_caption.find("3 张图片暂时无法获取"),
            std::string::npos);
  EXPECT_EQ(std::ranges::count_if(
                output,
                [](const auto &segment) {
                  return segment.type == "text" &&
                         segment.data.value("text", "").find("整体发送失败") !=
                             std::string::npos;
                }),
            0);
}

TEST(BridgeActorTest, QqForwardMediaFailureShowsStageAndStableReason) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  MultipartFallbackTelegramBot telegram_bot;
  RetryTestQQBot qq_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations(qq_bot, telegram_bot), config, nullptr,
      blocking_executor,
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        EXPECT_EQ(media.size(), 2U);
        co_return std::vector<bridge::qq::MediaDownloadResult>{
            {.image = bridge::qq::DownloadedImage{.type = "photo",
                                                  .filename = "qq-media-0.png",
                                                  .mime_type = "image/png",
                                                  .data = "download-one"}},
            {.image = bridge::qq::DownloadedImage{.type = "photo",
                                                  .filename = "qq-media-1.png",
                                                  .mime_type = "image/png",
                                                  .data = "download-two"}},
        };
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      preserve_test_images);
  qq_bot.forward_response = R"({
    "status":"ok",
    "data":{"messages":[{
      "sender":{"nickname":"sender"},
      "content":[
        {"type":"image","data":{"url":"invalid://one"}},
        {"type":"image","data":{"url":"invalid://two"}}
      ]
    }]}
  })";
  telegram_bot.multipart_fails.store(true, std::memory_order_release);
  telegram_bot.multipart_error =
      R"(HTTP request failed: 400: {"ok":false,"description":"Bad Request: IMAGE_PROCESS_FAILED"})";
  obcx::common::MessageSegment forward_segment{
      .type = "forward", .data = {{"id", "forward-with-bad-image"}}};
  obcx::common::Message output;

  asio::io_context ioc;
  auto future = asio::co_spawn(ioc,
                               formatter.process_forward_message(
                                   forward_segment, "tg-group", -1, output),
                               asio::use_future);
  ioc.run();
  future.get();

  EXPECT_EQ(telegram_bot.url_send_calls.load(), 1);
  EXPECT_EQ(telegram_bot.multipart_send_calls.load(), 1);
  const auto failure = std::ranges::find_if(output, [](const auto &segment) {
    return segment.type == "text" &&
           segment.data.value("text", "").find("整体发送失败") !=
               std::string::npos;
  });
  ASSERT_NE(failure, output.end());
  const auto text = failure->data.value("text", "");
  EXPECT_NE(text.find("阶段=multipart_upload"), std::string::npos);
  EXPECT_NE(text.find("原因=image_process_failed"), std::string::npos);
  EXPECT_NE(text.find("已替换=0/2"), std::string::npos);
  EXPECT_EQ(text.find("HTTP request failed"), std::string::npos);

  telegram_bot.multipart_error =
      R"(HTTP request failed: 400: {"ok":false,"description":"Bad Request: PHOTO_INVALID_DIMENSIONS"})";
  obcx::common::Message dimension_output;
  asio::io_context dimension_ioc;
  auto dimension_future =
      asio::co_spawn(dimension_ioc,
                     formatter.process_forward_message(
                         forward_segment, "tg-group", -1, dimension_output),
                     asio::use_future);
  dimension_ioc.run();
  dimension_future.get();
  blocking_executor->shutdown();

  const auto dimension_failure =
      std::ranges::find_if(dimension_output, [](const auto &segment) {
        return segment.type == "text" &&
               segment.data.value("text", "").find("整体发送失败") !=
                   std::string::npos;
      });
  ASSERT_NE(dimension_failure, dimension_output.end());
  const auto dimension_text = dimension_failure->data.value("text", "");
  EXPECT_NE(dimension_text.find("原因=invalid_dimensions"), std::string::npos);
  EXPECT_EQ(dimension_text.find("PHOTO_INVALID_DIMENSIONS"), std::string::npos);
}

TEST(BridgeActorTest, QqMultipartFallbackDownloadsSanitizedPlaceholderOnce) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  std::atomic_int download_calls = 0;
  MultipartFallbackTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor,
      [&download_calls](
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        download_calls.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(media.size(), 1U);
        co_return std::vector<bridge::qq::MediaDownloadResult>{
            {.image = bridge::qq::DownloadedImage{.type = "photo",
                                                  .filename = "placeholder.png",
                                                  .mime_type = "image/png",
                                                  .data = "placeholder-once"}}};
      },
      {}, preserve_test_images);
  const std::vector<obcx::common::MessageSegment> image_segments = {
      {.type = "image", .data = {{"url", "invalid://one"}}},
      {.type = "image", .data = {{"url", "invalid://two"}}},
  };
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", false,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-sanitized-placeholder-once";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();
  blocking_executor->shutdown();

  EXPECT_TRUE(result.sent);
  EXPECT_EQ(download_calls.load(), 1);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 2U);
  EXPECT_EQ(telegram_bot.uploaded_media[0].data, "placeholder-once");
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "placeholder-once");
  EXPECT_NE(telegram_bot.last_caption.find("2 张图片"), std::string::npos);
}

TEST(BridgeActorTest,
     QqMultipartFallbackReplacesFailedItemWithoutDroppingValidPeer) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  std::atomic_int batch_downloads = 0;
  std::atomic_int placeholder_downloads = 0;
  MultipartFallbackTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor,
      [&batch_downloads, &placeholder_downloads](
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        if (media.size() == 1) {
          placeholder_downloads.fetch_add(1, std::memory_order_relaxed);
          co_return std::vector<bridge::qq::MediaDownloadResult>{
              {.image =
                   bridge::qq::DownloadedImage{.type = "photo",
                                               .filename = "placeholder.png",
                                               .mime_type = "image/png",
                                               .data = "placeholder-data"}}};
        }
        batch_downloads.fetch_add(1, std::memory_order_relaxed);
        co_return std::vector<bridge::qq::MediaDownloadResult>{
            {.image = bridge::qq::DownloadedImage{.type = "photo",
                                                  .filename = "qq-media-0.jpg",
                                                  .mime_type = "image/jpeg",
                                                  .data = "download-one"}},
            {.failure = bridge::qq::MediaDownloadFailure::OverLimit,
             .diagnostic = "response exceeds configured media limit"},
        };
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      preserve_test_images);
  const std::vector<obcx::common::MessageSegment> image_segments = {
      {.type = "image", .data = {{"url", "invalid://one"}}},
      {.type = "image", .data = {{"url", "invalid://two"}}},
  };
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", false,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-partial-multipart-fallback";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();
  blocking_executor->shutdown();

  EXPECT_TRUE(result.sent);
  EXPECT_EQ(batch_downloads.load(), 1);
  EXPECT_EQ(placeholder_downloads.load(), 1);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 2U);
  EXPECT_EQ(telegram_bot.uploaded_media[0].data, "download-one");
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "placeholder-data");
  EXPECT_NE(telegram_bot.last_caption.find("1 张图片"), std::string::npos);
}

TEST(BridgeActorTest,
     QqMultipartFallbackUsesOneEmbeddedPlaceholderForSeveralFailures) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  std::atomic_int placeholder_downloads = 0;
  MultipartFallbackTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor,
      [&placeholder_downloads](
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &media)
          -> asio::awaitable<std::vector<bridge::qq::MediaDownloadResult>> {
        if (media.size() == 1) {
          placeholder_downloads.fetch_add(1, std::memory_order_relaxed);
          co_return std::vector<bridge::qq::MediaDownloadResult>{
              {.failure = bridge::qq::MediaDownloadFailure::Transport,
               .diagnostic = "media transport failed"}};
        }
        co_return std::vector<bridge::qq::MediaDownloadResult>{
            {.failure = bridge::qq::MediaDownloadFailure::OverLimit,
             .diagnostic = "response exceeds configured media limit"},
            {.image = bridge::qq::DownloadedImage{.type = "photo",
                                                  .filename = "qq-media-1.jpg",
                                                  .mime_type = "image/jpeg",
                                                  .data = "download-middle"}},
            {.failure = bridge::qq::MediaDownloadFailure::InvalidImage,
             .diagnostic = "media response is not a recognized image"},
        };
      },
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      },
      preserve_test_images);
  const std::vector<obcx::common::MessageSegment> image_segments = {
      {.type = "image", .data = {{"url", "invalid://one"}}},
      {.type = "image", .data = {{"url", "invalid://two"}}},
      {.type = "image", .data = {{"url", "invalid://three"}}},
  };
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", false,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-embedded-placeholder-fallback";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();
  blocking_executor->shutdown();

  EXPECT_TRUE(result.sent);
  EXPECT_EQ(placeholder_downloads.load(), 1);
  ASSERT_EQ(telegram_bot.uploaded_media.size(), 3U);
  EXPECT_EQ(telegram_bot.uploaded_media[1].data, "download-middle");
  EXPECT_EQ(telegram_bot.uploaded_media[0].data,
            telegram_bot.uploaded_media[2].data);
  ASSERT_GT(telegram_bot.uploaded_media[0].data.size(), 100U);
  const auto placeholder =
      bridge::qq::inspect_photo_dimensions(telegram_bot.uploaded_media[0].data);
  EXPECT_EQ(placeholder.status, bridge::qq::PhotoDimensionStatus::Compliant);
  EXPECT_EQ(placeholder.dimensions.width, 256U);
  EXPECT_EQ(placeholder.dimensions.height, 96U);
  EXPECT_NE(telegram_bot.last_caption.find("2 张图片"), std::string::npos);
}

TEST(BridgeActorTest, QqForwardKeepsElevenImagesInValidNineAndTwoBatches) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  RetryTestQQBot qq_bot;
  RetryTestTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations(qq_bot, telegram_bot), config, nullptr,
      blocking_executor, {},
      [](const bridge::BridgeConfig &,
         const std::vector<std::pair<std::string, std::string>> &media,
         std::vector<std::string> &replaced)
          -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
        replaced.clear();
        co_return media;
      });
  nlohmann::json messages = nlohmann::json::array();
  for (std::size_t index = 0; index < 11; ++index) {
    messages.push_back(
        {{"sender", {{"nickname", "node"}}},
         {"content",
          nlohmann::json::array(
              {{{"type", "image"},
                {"data", {{"url", fmt::format("invalid://{}", index)}}}}})}});
  }
  qq_bot.forward_response =
      nlohmann::json{{"status", "ok"}, {"data", {{"messages", messages}}}}
          .dump();
  obcx::common::Message output;
  const obcx::common::MessageSegment forward_segment{
      .type = "forward", .data = {{"id", "eleven-forward-images"}}};

  asio::io_context ioc;
  auto future = asio::co_spawn(ioc,
                               formatter.process_forward_message(
                                   forward_segment, "tg-group", -1, output),
                               asio::use_future);
  ioc.run();
  future.get();
  blocking_executor->shutdown();

  EXPECT_EQ(telegram_bot.send_media_group_calls.load(), 2);
  EXPECT_EQ(telegram_bot.media_group_sizes, (std::vector<size_t>{9U, 2U}));
}

TEST(BridgeActorTest, QqMediaGroupKeepsElevenImagesInValidNineAndTwoBatches) {
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->bridge_files_dir = "/tmp/bridge_files";
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  RetryTestTelegramBot telegram_bot;
  bridge::qq::QQMessageFormatter formatter(
      bridge_operations_for_telegram(telegram_bot), config, nullptr,
      blocking_executor);
  std::vector<obcx::common::MessageSegment> image_segments;
  image_segments.reserve(11);
  for (size_t index = 0; index < 11; ++index) {
    image_segments.push_back(
        {.type = "image",
         .data = {{"url", fmt::format("invalid://image-{}", index)}}});
  }
  const bridge::GroupBridgeConfig bridge_config("tg-group", "qq-group", false,
                                                false, true, true);
  obcx::common::MessageEvent event;
  event.message_id = "qq-eleven-image-album";

  asio::io_context ioc;
  auto future = asio::co_spawn(
      ioc,
      formatter.send_media_group(image_segments, {}, "tg-group", -1, "sender",
                                 &bridge_config, {}, event),
      asio::use_future);
  ioc.run();
  const auto result = future.get();
  blocking_executor->shutdown();

  EXPECT_TRUE(result.sent);
  EXPECT_EQ(telegram_bot.send_media_group_calls.load(), 2);
  EXPECT_EQ(telegram_bot.media_group_sizes, (std::vector<size_t>{9U, 2U}));
}

TEST(BridgeActorTest,
     DeferredTelegramMediaGroupOwnsItsMappingsWithoutActorDirectWrite) {
  const auto db_path = temp_db_path("deferred_media_group_counts");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();

  auto config = std::make_shared<bridge::BridgeConfig>();
  config->group_map.emplace(
      "tg-group", bridge::GroupBridgeConfig("tg-group", "qq-group", false,
                                            false, true, true));
  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  asio::io_context ioc;
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  qq_bot->always_succeed.store(true, std::memory_order_release);
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  auto operations = bridge_operations(*qq_bot, *telegram_bot);
  auto handler = std::make_shared<bridge::TelegramHandler>(
      operations, config, nullptr, ioc.get_executor(), repository, nullptr,
      blocking_executor);

  auto media_group_event = [](std::string message_id, std::string text) {
    obcx::common::MessageEvent event;
    event.message_id = std::move(message_id);
    event.message_type = "group";
    event.group_id = "tg-group";
    event.raw_message = text;
    event.data = {{"media_group_id", "album-1"}};
    event.message.push_back(obcx::common::MessageSegment{
        .type = "text", .data = {{"text", std::move(text)}}});
    return event;
  };

  repository->reset_message_mapping_operation_counts();
  auto future = asio::co_spawn(
      ioc,
      [handler, telegram_bot, qq_bot,
       first = media_group_event("tg-album-1", "first"),
       second = media_group_event("tg-album-2", "second")]() mutable
          -> asio::awaitable<std::pair<bridge::DirectForwardOutcome,
                                       bridge::DirectForwardOutcome>> {
        auto first_outcome = co_await handler->forward_to_qq(std::move(first));
        auto second_outcome =
            co_await handler->forward_to_qq(std::move(second));
        handler->flush_pending_media_groups();
        co_return std::pair{std::move(first_outcome),
                            std::move(second_outcome)};
      },
      asio::use_future);
  ioc.run();
  const auto outcomes = future.get();

  EXPECT_EQ(outcomes.first.disposition,
            bridge::DirectForwardDisposition::NotForwarded);
  EXPECT_EQ(outcomes.second.disposition,
            bridge::DirectForwardDisposition::NotForwarded);
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);
  const auto counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(counts.direct_forward_writes, 0U);
  EXPECT_EQ(counts.retry_completion_writes, 0U);
  EXPECT_EQ(counts.deferred_media_group_writes, 2U);
  EXPECT_EQ(counts.post_send_recovery_reads, 0U);
  EXPECT_EQ(repository->get_target_message_id("telegram", "tg-album-1", "qq"),
            "9001");
  EXPECT_EQ(repository->get_target_message_id("telegram", "tg-album-2", "qq"),
            "9001");

  handler.reset();
  blocking_executor->shutdown();
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

TEST(BridgeActorTest, PossiblySubmittedInitialSendsAreNeverQueued) {
  const auto db_path = temp_db_path("possibly_submitted_initial");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(db_path)});
  auto qq_bot = std::make_shared<RetryTestQQBot>();
  auto telegram_bot = std::make_shared<RetryTestTelegramBot>();
  qq_bot->force_malformed_send.store(true, std::memory_order_release);
  telegram_bot->force_malformed_send.store(true, std::memory_order_release);
  auto services = bridge_retry_services(db_manager, qq_bot, telegram_bot, true);

  const auto qq_result = run_actor(
      services, bridge_message_stored("qq", "qq-unknown-1", "qq-group"));
  const auto telegram_result = run_actor(
      services, bridge_message_stored("telegram", "tg-unknown-1", "tg-group"));

  EXPECT_FALSE(qq_result.ok());
  EXPECT_FALSE(telegram_result.ok());
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 1);
  EXPECT_EQ(qq_bot->send_group_calls.load(), 1);
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());
  EXPECT_FALSE(
      repository->get_target_message_id("qq", "qq-unknown-1", "telegram")
          .has_value());
  EXPECT_FALSE(
      repository->get_target_message_id("telegram", "tg-unknown-1", "qq")
          .has_value());

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

TEST(BridgeActorTest, TelegramGroupRetryUsesOperationClientAndPersistsMapping) {
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
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("qq", "qq-group-retry-success", "qq-group"),
      repository, "qq", "qq-group-retry-success", "telegram");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "8001");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 2);
  EXPECT_EQ(telegram_bot->send_topic_calls.load(), 0);
  const auto operation_counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(operation_counts.direct_forward_writes, 0U);
  EXPECT_EQ(operation_counts.retry_completion_writes, 1U);
  EXPECT_EQ(operation_counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(operation_counts.post_send_recovery_reads, 0U);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, TelegramTopicRetryUsesOperationClientAndPersistsMapping) {
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
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("qq", "qq-topic-retry-success", "qq-group"),
      repository, "qq", "qq-topic-retry-success", "telegram");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "8002");
  EXPECT_EQ(telegram_bot->send_group_calls.load(), 0);
  EXPECT_EQ(telegram_bot->send_topic_calls.load(), 2);
  const auto operation_counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(operation_counts.direct_forward_writes, 0U);
  EXPECT_EQ(operation_counts.retry_completion_writes, 1U);
  EXPECT_EQ(operation_counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(operation_counts.post_send_recovery_reads, 0U);
  EXPECT_TRUE(
      repository
          ->get_pending_message_retries(
              std::chrono::system_clock::now() + std::chrono::hours{1}, 10)
          .empty());

  std::filesystem::remove(db_path);
}

TEST(BridgeActorTest, QqGroupRetryUsesOperationClientAndPersistsMapping) {
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
  repository->initialize_schema();
  repository->reset_message_mapping_operation_counts();
  services->register_service<bridge::BridgeStateRepository>(repository);

  const auto result = run_actor_until_retry(
      services,
      bridge_message_stored("telegram", "tg-group-retry-success", "tg-group"),
      repository, "telegram", "tg-group-retry-success", "qq");

  EXPECT_FALSE(result.initial_result.ok());
  ASSERT_TRUE(result.target_message_id.has_value());
  EXPECT_EQ(result.target_message_id.value(), "9001");
  EXPECT_EQ(qq_bot->send_group_calls.load(), 2);
  const auto operation_counts = repository->message_mapping_operation_counts();
  EXPECT_EQ(operation_counts.direct_forward_writes, 0U);
  EXPECT_EQ(operation_counts.retry_completion_writes, 1U);
  EXPECT_EQ(operation_counts.deferred_media_group_writes, 0U);
  EXPECT_EQ(operation_counts.post_send_recovery_reads, 0U);
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

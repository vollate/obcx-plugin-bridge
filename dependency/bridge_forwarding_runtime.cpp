#include "bridge_forwarding_runtime.hpp"

#include "bridge_message_event_adapter.hpp"
#include "config.hpp"
#include "qq/handler.hpp"
#include "retry_queue_manager.hpp"
#include "telegram/handler.hpp"

#include <common/logger.hpp>
#include <interfaces/telegram_bot.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bridge {
namespace {

auto source_platform(const obcx::core::MessageEnvelope &message)
    -> std::string {
  if (!message.source_platform.empty()) {
    return message.source_platform;
  }
  if (message.payload.is_object() &&
      message.payload.contains("source_platform") &&
      message.payload["source_platform"].is_string()) {
    return message.payload["source_platform"].get<std::string>();
  }
  return {};
}

auto target_platform_for(const std::string &platform) -> std::string {
  if (platform == "qq") {
    return "telegram";
  }
  if (platform == "telegram") {
    return "qq";
  }
  return {};
}

auto require_bot(const std::shared_ptr<obcx::core::BotRegistry> &registry,
                 const std::string &platform) -> obcx::core::RegisteredBot {
  if (!registry) {
    throw std::runtime_error("bridge forwarding requires BotRegistry service");
  }
  auto bot = registry->find_bot(platform);
  if (!bot.has_value() || bot->bot == nullptr) {
    throw std::runtime_error("bridge forwarding missing bot for platform " +
                             platform);
  }
  return bot.value();
}

auto require_source_bot(
    const std::shared_ptr<obcx::core::BotRegistry> &registry,
    const obcx::command::CommandInvocation &invocation)
    -> obcx::core::RegisteredBot {
  if (registry && !invocation.source_bot.empty()) {
    if (auto bot = registry->find_bot(invocation.source_platform,
                                      invocation.source_bot)) {
      return *bot;
    }
  }
  return require_bot(registry, invocation.source_platform);
}

auto require_source_bot(
    const std::shared_ptr<obcx::core::BotRegistry> &registry,
    const obcx::core::MessageEnvelope &message) -> obcx::core::RegisteredBot {
  if (registry && !message.source_bot.empty()) {
    if (auto bot =
            registry->find_bot(message.source_platform, message.source_bot)) {
      return *bot;
    }
  }
  return require_bot(registry, message.source_platform);
}

auto command_message(const obcx::command::CommandInvocation &invocation)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope message;
  message.id = invocation.source_message_id;
  message.type = "obcx::message_store::events::MessageStored";
  message.source_platform = invocation.source_platform;
  message.source_bot = invocation.source_bot;
  message.conversation_id = invocation.conversation_id;
  message.payload = invocation.source_event;
  if (invocation.source_event.is_object() &&
      invocation.source_event.contains("payload") &&
      invocation.source_event.at("payload").is_object()) {
    message.raw = invocation.source_event.at("payload");
  }
  return message;
}

auto is_telegram_edit(const obcx::common::MessageEvent &event) -> bool {
  return event.sub_type == "edited" || (event.data.contains("is_edited") &&
                                        event.data["is_edited"].is_boolean() &&
                                        event.data["is_edited"].get<bool>());
}

auto retry_policy(const BridgeConfig &config) -> RetryQueuePolicy {
  return RetryQueuePolicy{
      .message_retry_base_interval_sec = config.message_retry_base_interval_sec,
      .media_retry_base_interval_sec = config.media_retry_base_interval_sec,
      .retry_queue_check_interval_sec = config.retry_queue_check_interval_sec,
      .max_retry_interval_sec = config.max_retry_interval_sec,
  };
}

auto response_message_id(const std::string &response, const char *container)
    -> std::optional<std::string> {
  if (response.empty()) {
    return std::nullopt;
  }
  const auto document = nlohmann::json::parse(response, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      !document.contains(container) || !document.at(container).is_object() ||
      !document.at(container).contains("message_id")) {
    return std::nullopt;
  }
  const auto &message_id = document.at(container).at("message_id");
  if (message_id.is_string()) {
    return message_id.get<std::string>();
  }
  if (message_id.is_number_integer()) {
    return std::to_string(message_id.get<std::int64_t>());
  }
  if (message_id.is_number_unsigned()) {
    return std::to_string(message_id.get<std::uint64_t>());
  }
  return std::nullopt;
}

auto telegram_retry_callback(
    const std::shared_ptr<obcx::core::BotRegistry> &bot_registry)
    -> RetryQueueManager::MessageSendCallback {
  return [bot_registry](const MessageRetryEntry &retry,
                        const obcx::common::Message &message)
             -> boost::asio::awaitable<std::optional<std::string>> {
    const auto registration =
        bot_registry ? bot_registry->find_bot("telegram") : std::nullopt;
    if (!registration.has_value() || !registration->bot) {
      OBCX_WARN("Telegram bot unavailable for bridge message retry");
      co_return std::nullopt;
    }

    try {
      std::string response;
      if (retry.target_topic_id > 0) {
        auto *telegram =
            dynamic_cast<obcx::core::ITelegramBot *>(registration->bot.get());
        if (telegram == nullptr) {
          OBCX_WARN("Registered Telegram bot lacks topic-send capability");
          co_return std::nullopt;
        }
        response = co_await telegram->send_topic_message(
            retry.group_id, retry.target_topic_id, message);
      } else {
        response = co_await registration->bot->send_group_message(
            retry.group_id, message);
      }
      co_return response_message_id(response, "result");
    } catch (const std::exception &) {
      OBCX_WARN("Telegram bridge message retry send failed");
      co_return std::nullopt;
    }
  };
}

auto qq_retry_callback(
    const std::shared_ptr<obcx::core::BotRegistry> &bot_registry)
    -> RetryQueueManager::MessageSendCallback {
  return [bot_registry](const MessageRetryEntry &retry,
                        const obcx::common::Message &message)
             -> boost::asio::awaitable<std::optional<std::string>> {
    const auto registration =
        bot_registry ? bot_registry->find_bot("qq") : std::nullopt;
    if (!registration.has_value() || !registration->bot) {
      OBCX_WARN("QQ bot unavailable for bridge message retry");
      co_return std::nullopt;
    }

    try {
      const auto response = co_await registration->bot->send_group_message(
          retry.group_id, message);
      co_return response_message_id(response, "data");
    } catch (const std::exception &) {
      OBCX_WARN("QQ bridge message retry send failed");
      co_return std::nullopt;
    }
  };
}

} // namespace

class RetryQueueWorker {
public:
  RetryQueueWorker(std::shared_ptr<obcx::core::BotRegistry> bot_registry,
                   const BridgeConfig &config,
                   std::shared_ptr<BridgeStateRepository> state_repository)
      : work_guard_(std::make_unique<WorkGuard>(
            boost::asio::make_work_guard(io_context_))),
        manager_(std::make_shared<RetryQueueManager>(
            io_context_, std::move(state_repository), retry_policy(config))) {
    manager_->register_message_send_callback(
        "telegram", telegram_retry_callback(bot_registry));
    manager_->register_message_send_callback("qq",
                                             qq_retry_callback(bot_registry));
    manager_->restore_persisted_message_retries();
    manager_->start();
    worker_thread_ = std::thread([this] {
      OBCX_INFO("Bridge retry worker thread started");
      io_context_.run();
      OBCX_INFO("Bridge retry worker thread stopped");
    });
  }

  RetryQueueWorker(const RetryQueueWorker &) = delete;
  auto operator=(const RetryQueueWorker &) -> RetryQueueWorker & = delete;
  ~RetryQueueWorker() { shutdown(); }

  [[nodiscard]] auto manager() const -> std::shared_ptr<RetryQueueManager> {
    return manager_;
  }

  void shutdown() noexcept {
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
      return;
    }

    try {
      const auto manager = manager_;
      const auto stopped =
          manager ? manager->stopped() : std::shared_future<void>{};
      if (manager) {
        boost::asio::post(io_context_, [manager] { manager->stop(); });
      }

      bool timed_out = false;
      if (stopped.valid() && stopped.wait_for(std::chrono::seconds{35}) !=
                                 std::future_status::ready) {
        timed_out = true;
        OBCX_ERROR("Bridge retry worker did not stop before its deadline");
      }

      work_guard_.reset();
      if (timed_out) {
        io_context_.stop();
      }
      if (worker_thread_.joinable()) {
        worker_thread_.join();
      }
      io_context_.stop();
      manager_.reset();
    } catch (const std::exception &error) {
      OBCX_ERROR("Bridge retry worker shutdown failed: {}", error.what());
      io_context_.stop();
      if (worker_thread_.joinable()) {
        try {
          worker_thread_.join();
        } catch (...) {
        }
      }
      manager_.reset();
    } catch (...) {
      io_context_.stop();
      if (worker_thread_.joinable()) {
        try {
          worker_thread_.join();
        } catch (...) {
        }
      }
      manager_.reset();
    }
  }

private:
  using WorkGuard =
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

  boost::asio::io_context io_context_;
  std::unique_ptr<WorkGuard> work_guard_;
  std::shared_ptr<RetryQueueManager> manager_;
  std::thread worker_thread_;
  std::atomic_bool shutting_down_{false};
};

BridgeForwardingRuntime::BridgeForwardingRuntime(
    std::shared_ptr<obcx::core::BotRegistry> bot_registry,
    std::shared_ptr<const BridgeConfig> config,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository,
    boost::asio::any_io_executor buffer_executor)
    : bot_registry_(std::move(bot_registry)), config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)) {
  if (!config_) {
    throw std::invalid_argument("bridge forwarding requires configuration");
  }

  std::shared_ptr<RetryQueueManager> retry_manager;
  if (config_->enable_retry_queue) {
    try {
      retry_worker_ = std::make_unique<RetryQueueWorker>(
          bot_registry_, *config_, state_repository_);
      retry_manager = retry_worker_->manager();
    } catch (const std::exception &error) {
      throw BridgeRetryUnavailable(
          std::string{"bridge retry worker initialization failed: "} +
          error.what());
    }
  }

  qq_handler_ = std::make_shared<QQHandler>(
      config_, retry_manager, state_repository_, received_message_repository_);
  telegram_handler_ = std::make_shared<TelegramHandler>(
      config_, std::move(retry_manager), std::move(buffer_executor),
      state_repository_, received_message_repository_);
}

BridgeForwardingRuntime::~BridgeForwardingRuntime() { shutdown(); }

void BridgeForwardingRuntime::shutdown() noexcept {
  bool expected = false;
  if (!shutting_down_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
    return;
  }
  if (telegram_handler_) {
    try {
      telegram_handler_->flush_pending_media_groups();
    } catch (...) {
    }
  }
  if (retry_worker_) {
    retry_worker_->shutdown();
  }
}

auto BridgeForwardingRuntime::forward_message(
    const obcx::core::MessageEnvelope &message)
    -> boost::asio::awaitable<BridgeForwardResult> {
  if (shutting_down_.load(std::memory_order_acquire)) {
    throw std::runtime_error("bridge forwarding runtime is shutting down");
  }
  const auto event = message_event_from_message_stored(message);
  if (!event.has_value()) {
    throw std::runtime_error(
        "MessageStored cannot be converted to MessageEvent");
  }

  const auto from_platform = source_platform(message);
  const auto to_platform = target_platform_for(from_platform);
  if (to_platform.empty()) {
    throw std::runtime_error("unsupported bridge source platform " +
                             from_platform);
  }

  const auto source_bot = require_bot(bot_registry_, from_platform);
  const auto target_bot = require_bot(bot_registry_, to_platform);

  if (from_platform == "qq") {
    co_await qq_handler_->forward_to_telegram(*target_bot.bot, *source_bot.bot,
                                              event.value());
  } else if (is_telegram_edit(event.value())) {
    co_await telegram_handler_->handle_message_edited(
        *source_bot.bot, *target_bot.bot, event.value());
  } else {
    co_await telegram_handler_->forward_to_qq(*source_bot.bot, *target_bot.bot,
                                              event.value());
  }

  auto target_message_id = state_repository_->get_target_message_id(
      from_platform, event->message_id, to_platform);
  if (!target_message_id.has_value()) {
    throw std::runtime_error(
        "bridge forwarding completed without persisted mapping");
  }

  co_return BridgeForwardResult{.source_platform = from_platform,
                                .source_message_id = event->message_id,
                                .target_platform = to_platform,
                                .target_bot = target_bot.bot_id,
                                .target_message_id = target_message_id.value()};
}

auto BridgeForwardingRuntime::handle_command(
    const obcx::command::CommandInvocation &invocation)
    -> boost::asio::awaitable<bool> {
  if (shutting_down_.load(std::memory_order_acquire)) {
    throw std::runtime_error("bridge forwarding runtime is shutting down");
  }
  const auto event =
      message_event_from_message_stored(command_message(invocation));
  if (!event || event->message_type != "group" ||
      !event->group_id.has_value()) {
    co_return false;
  }

  const auto source = require_source_bot(bot_registry_, invocation);
  if (invocation.source_platform == "telegram") {
    if (invocation.name != "recall" && invocation.name != "checkalive" &&
        invocation.name != "poke") {
      co_return false;
    }
    const auto target = require_bot(bot_registry_, "qq");
    const auto telegram_group_id = *event->group_id;
    const auto *mapping = config_->bridge_config(telegram_group_id);
    if (mapping == nullptr) {
      co_return false;
    }
    std::string qq_group_id;
    if (mapping->mode == BridgeMode::GROUP_TO_GROUP) {
      qq_group_id = mapping->qq_group_id;
    } else {
      const auto topic_id =
          event->data.value("message_thread_id", std::int64_t{-1});
      qq_group_id = config_->qq_group_id_for_topic(telegram_group_id, topic_id);
    }
    if (qq_group_id.empty()) {
      co_return false;
    }
    if (invocation.name == "recall") {
      co_await telegram_handler_->handle_recall_command(
          *source.bot, *target.bot, *event, qq_group_id);
    } else if (invocation.name == "checkalive") {
      co_await telegram_handler_->handle_checkalive_command(
          *source.bot, *target.bot, *event, qq_group_id);
    } else {
      co_await telegram_handler_->handle_poke_command(*source.bot, *target.bot,
                                                      *event, qq_group_id);
    }
    co_return true;
  }

  if (invocation.source_platform == "qq" && invocation.name == "checkalive") {
    const auto target = require_bot(bot_registry_, "telegram");
    const auto [telegram_group_id, topic_id] =
        config_->tg_group_and_topic_id(*event->group_id);
    (void)topic_id;
    if (telegram_group_id.empty()) {
      co_return false;
    }
    co_await qq_handler_->handle_checkalive_command(*target.bot, *source.bot,
                                                    *event, telegram_group_id);
    co_return true;
  }
  co_return false;
}

auto BridgeForwardingRuntime::handle_notice(
    const obcx::core::MessageEnvelope &message)
    -> boost::asio::awaitable<bool> {
  if (shutting_down_.load(std::memory_order_acquire)) {
    throw std::runtime_error("bridge forwarding runtime is shutting down");
  }
  if (message.source_platform != "qq") {
    co_return false;
  }
  const auto notice = notice_event_from_raw_notice(message);
  if (!notice.has_value()) {
    co_return false;
  }

  const auto source = require_source_bot(bot_registry_, message);
  const auto target = require_bot(bot_registry_, "telegram");
  obcx::common::Event event = *notice;
  co_await qq_handler_->handle_recall_event(*target.bot, *source.bot,
                                            std::move(event));
  co_return true;
}

} // namespace bridge

#include "bridge_forwarding_runtime.hpp"

#include "bridge_message_event_adapter.hpp"
#include "config.hpp"
#include "qq/handler.hpp"
#include "retry_queue_manager.hpp"
#include "telegram/handler.hpp"
#include "telegram/media_group_buffer.hpp"

#include <common/logger.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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

auto telegram_retry_callback(
    const std::shared_ptr<BridgeBotOperations> &operations,
    BridgeInstallationPair pair) -> RetryQueueManager::MessageSendCallback {
  return [operations,
          pair = std::move(pair)](const MessageRetryEntry &retry,
                                  const obcx::common::Message &message)
             -> boost::asio::awaitable<MessageSendOutcome> {
    if (retry.target_installation !=
        operations->telegram_installation().installation_id) {
      co_return MessageSendOutcome::terminal("target_installation_mismatch");
    }
    try {
      if (native_conversation_id("telegram", retry.target_conversation_id) !=
              retry.group_id ||
          native_conversation_id("qq", retry.source_conversation_id) !=
              retry.source_group_id) {
        co_return MessageSendOutcome::terminal("target_conversation_mismatch");
      }
      const auto [target_group, target_topic] =
          pair.tg_group_and_topic_id(retry.source_group_id);
      if (target_group != retry.group_id ||
          target_topic != retry.target_topic_id) {
        co_return MessageSendOutcome::terminal(
            "target_conversation_unavailable");
      }
      if (retry.target_topic_id > 0) {
        co_return MessageSendOutcome::completed(
            co_await operations->send_telegram_topic(
                retry.group_id, retry.target_topic_id, message));
      }
      co_return MessageSendOutcome::completed(
          co_await operations->send_telegram_group(retry.group_id, message));
    } catch (const BridgeBotOperationFailure &failure) {
      const auto &error = failure.error();
      if (error.submission_safety ==
          obcx::bot::SubmissionSafety::PossiblySubmitted) {
        co_return MessageSendOutcome::unknown(
            std::string{obcx::bot::error_code_id(error.code)});
      }
      if (error.retryable) {
        co_return MessageSendOutcome::retryable(
            std::string{obcx::bot::error_code_id(error.code)});
      }
      co_return MessageSendOutcome::terminal(
          std::string{obcx::bot::error_code_id(error.code)});
    } catch (...) {
      co_return MessageSendOutcome::unknown("unclassified_exception");
    }
  };
}

auto qq_retry_callback(const std::shared_ptr<BridgeBotOperations> &operations,
                       BridgeInstallationPair pair)
    -> RetryQueueManager::MessageSendCallback {
  return [operations,
          pair = std::move(pair)](const MessageRetryEntry &retry,
                                  const obcx::common::Message &message)
             -> boost::asio::awaitable<MessageSendOutcome> {
    if (retry.target_installation !=
        operations->onebot11_installation().installation_id) {
      co_return MessageSendOutcome::terminal("target_installation_mismatch");
    }
    try {
      if (native_conversation_id("qq", retry.target_conversation_id) !=
              retry.group_id ||
          native_conversation_id("telegram", retry.source_conversation_id) !=
              retry.source_group_id) {
        co_return MessageSendOutcome::terminal("target_conversation_mismatch");
      }
      const auto *mapping = pair.bridge_config(retry.source_group_id);
      bool route_available = false;
      if (mapping != nullptr && mapping->mode == BridgeMode::GROUP_TO_GROUP) {
        route_available = mapping->qq_group_id == retry.group_id;
      } else if (mapping != nullptr) {
        route_available =
            std::ranges::any_of(mapping->topics, [&](const auto &topic) {
              return topic.qq_group_id == retry.group_id;
            });
      }
      if (!route_available) {
        co_return MessageSendOutcome::terminal(
            "target_conversation_unavailable");
      }
      co_return MessageSendOutcome::completed(
          co_await operations->send_onebot11_group(retry.group_id, message));
    } catch (const BridgeBotOperationFailure &failure) {
      const auto &error = failure.error();
      if (error.submission_safety ==
          obcx::bot::SubmissionSafety::PossiblySubmitted) {
        co_return MessageSendOutcome::unknown(
            std::string{obcx::bot::error_code_id(error.code)});
      }
      if (error.retryable) {
        co_return MessageSendOutcome::retryable(
            std::string{obcx::bot::error_code_id(error.code)});
      }
      co_return MessageSendOutcome::terminal(
          std::string{obcx::bot::error_code_id(error.code)});
    } catch (...) {
      co_return MessageSendOutcome::unknown("unclassified_exception");
    }
  };
}

} // namespace

class RetryQueueWorker {
public:
  RetryQueueWorker(
      const std::vector<std::shared_ptr<BridgeBotOperations>> &operations,
      const BridgeConfig &config,
      std::shared_ptr<BridgeStateRepository> state_repository)
      : work_guard_(std::make_unique<WorkGuard>(
            boost::asio::make_work_guard(io_context_))),
        manager_(std::make_shared<RetryQueueManager>(
            io_context_, std::move(state_repository), retry_policy(config))) {
    for (const auto &pair_operations : operations) {
      const auto *pair = config.pair(pair_operations->pair_id());
      if (pair == nullptr) {
        throw std::runtime_error("bridge retry pair is unavailable");
      }
      manager_->register_message_send_callback(
          pair_operations->telegram_installation().installation_id,
          telegram_retry_callback(pair_operations, *pair));
      manager_->register_message_send_callback(
          pair_operations->onebot11_installation().installation_id,
          qq_retry_callback(pair_operations, *pair));
    }
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

struct BridgeForwardingRuntime::PairRuntime {
  std::shared_ptr<BridgeBotOperations> operations;
  std::shared_ptr<QQHandler> qq_handler;
  std::shared_ptr<TelegramHandler> telegram_handler;
};

BridgeForwardingRuntime::BridgeForwardingRuntime(
    std::shared_ptr<obcx::bot::BotOperationClient> operation_client,
    std::shared_ptr<const BridgeConfig> config,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository,
    boost::asio::any_io_executor buffer_executor,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      blocking_executor_(std::move(blocking_executor)) {
  if (!config_) {
    throw std::invalid_argument("bridge forwarding requires configuration");
  }
  if (!operation_client) {
    throw std::invalid_argument("bridge forwarding requires operation client");
  }
  if (!blocking_executor_) {
    throw std::invalid_argument(
        "bridge forwarding requires BlockingExecutor service");
  }

  std::vector<std::shared_ptr<BridgeBotOperations>> operations;
  operations.reserve(config_->installation_pairs.size());
  for (const auto &[pair_id, pair] : config_->installation_pairs) {
    operations.push_back(std::make_shared<BridgeBotOperations>(
        operation_client, pair.telegram_installation,
        pair.onebot11_installation, pair_id));
  }

  std::shared_ptr<RetryQueueManager> retry_manager;
  if (config_->enable_retry_queue) {
    try {
      retry_worker_ = std::make_unique<RetryQueueWorker>(operations, *config_,
                                                         state_repository_);
      retry_manager = retry_worker_->manager();
    } catch (const std::exception &error) {
      throw BridgeRetryUnavailable(
          std::string{"bridge retry worker initialization failed: "} +
          error.what());
    }
  }

  media_group_buffer_ =
      std::make_shared<telegram::TGMediaGroupBuffer>(buffer_executor);
  for (const auto &pair_operations : operations) {
    auto runtime = std::make_unique<PairRuntime>();
    runtime->operations = pair_operations;
    runtime->qq_handler = std::make_shared<QQHandler>(
        pair_operations, config_, retry_manager, state_repository_,
        received_message_repository_, blocking_executor_);
    runtime->telegram_handler = std::make_shared<TelegramHandler>(
        pair_operations, config_, retry_manager, buffer_executor,
        state_repository_, received_message_repository_, blocking_executor_,
        media_group_buffer_);
    pair_runtimes_.emplace(pair_operations->pair_id(), std::move(runtime));
  }
}

BridgeForwardingRuntime::~BridgeForwardingRuntime() { shutdown(); }

void BridgeForwardingRuntime::shutdown() noexcept {
  bool expected = false;
  if (!shutting_down_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
    return;
  }
  if (media_group_buffer_) {
    try {
      media_group_buffer_->flush_all_now();
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
  const auto &pair =
      resolve_bridge_source_pair(*config_, from_platform, message.source_bot);
  const auto runtime = pair_runtimes_.find(pair.id);
  if (runtime == pair_runtimes_.end() || !runtime->second) {
    throw std::runtime_error("bridge pair runtime is unavailable: " + pair.id);
  }

  DirectForwardOutcome outcome;
  if (from_platform == "qq") {
    outcome = co_await runtime->second->qq_handler->forward_to_telegram(
        event.value());
  } else if (is_telegram_edit(event.value())) {
    outcome = co_await runtime->second->telegram_handler->handle_message_edited(
        event.value());
  } else {
    outcome = co_await runtime->second->telegram_handler->forward_to_qq(
        event.value());
  }

  const auto source_bot = from_platform == "telegram"
                              ? pair.telegram_installation
                              : pair.onebot11_installation;
  const auto target_bot = to_platform == "telegram"
                              ? pair.telegram_installation
                              : pair.onebot11_installation;
  co_return BridgeForwardResult{
      .disposition = outcome.disposition,
      .source_platform = std::move(outcome.source_platform),
      .source_bot = source_bot,
      .source_conversation_id = std::move(outcome.source_conversation_id),
      .source_message_id = std::move(outcome.source_message_id),
      .target_platform = std::move(outcome.target_platform),
      .target_bot = target_bot,
      .target_conversation_id = std::move(outcome.target_conversation_id),
      .target_message_id = std::move(outcome.target_message_id),
      .failure_message = std::move(outcome.failure_message),
      .failure_retryable = outcome.failure_retryable,
  };
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

  const auto &pair = resolve_bridge_source_pair(
      *config_, invocation.source_platform, invocation.source_bot);
  const auto runtime = pair_runtimes_.find(pair.id);
  if (runtime == pair_runtimes_.end() || !runtime->second) {
    throw std::runtime_error("bridge pair runtime is unavailable: " + pair.id);
  }

  if (invocation.source_platform == "telegram") {
    if (invocation.name != "recall" && invocation.name != "checkalive" &&
        invocation.name != "poke") {
      co_return false;
    }
    const auto telegram_group_id = *event->group_id;
    const auto *mapping = pair.bridge_config(telegram_group_id);
    if (mapping == nullptr) {
      co_return false;
    }
    std::string qq_group_id;
    if (mapping->mode == BridgeMode::GROUP_TO_GROUP) {
      qq_group_id = mapping->qq_group_id;
    } else {
      const auto topic_id =
          event->data.value("message_thread_id", std::int64_t{-1});
      qq_group_id = pair.qq_group_id_for_topic(telegram_group_id, topic_id);
    }
    if (qq_group_id.empty()) {
      co_return false;
    }
    if (invocation.name == "recall") {
      co_await runtime->second->telegram_handler->handle_recall_command(
          *event, qq_group_id);
    } else if (invocation.name == "checkalive") {
      co_await runtime->second->telegram_handler->handle_checkalive_command(
          *event, qq_group_id);
    } else {
      co_await runtime->second->telegram_handler->handle_poke_command(
          *event, qq_group_id);
    }
    co_return true;
  }

  if (invocation.source_platform == "qq" && invocation.name == "checkalive") {
    const auto [telegram_group_id, topic_id] =
        pair.tg_group_and_topic_id(*event->group_id);
    (void)topic_id;
    if (telegram_group_id.empty()) {
      co_return false;
    }
    co_await runtime->second->qq_handler->handle_checkalive_command(
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
  const auto &pair = resolve_bridge_source_pair(
      *config_, message.source_platform, message.source_bot);
  const auto runtime = pair_runtimes_.find(pair.id);
  if (runtime == pair_runtimes_.end() || !runtime->second) {
    throw std::runtime_error("bridge pair runtime is unavailable: " + pair.id);
  }
  const auto notice = notice_event_from_raw_notice(message);
  if (!notice.has_value()) {
    co_return false;
  }

  obcx::common::Event event = *notice;
  co_await runtime->second->qq_handler->handle_recall_event(std::move(event));
  co_return true;
}

} // namespace bridge

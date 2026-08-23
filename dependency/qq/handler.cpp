#include "qq/handler.hpp"
#include "bridge_state_repository.hpp"
#include "config.hpp"
#include "media_processor.hpp"
#include "qq/command_handler.hpp"
#include "qq/event_handler.hpp"
#include "qq/media_processor.hpp"
#include "qq/message_formatter.hpp"
#include "received_message_repository.hpp"
#include "retry_queue_manager.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge {

QQHandler::QQHandler(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const BridgeConfig> config,
    std::shared_ptr<RetryQueueManager> retry_manager,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : operations_(std::move(operations)), config_(std::move(config)),
      retry_manager_(std::move(retry_manager)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      blocking_executor_(std::move(blocking_executor)),
      media_processor_(std::make_unique<qq::QQMediaProcessor>(
          operations_, config_, state_repository_, blocking_executor_)),
      command_handler_(std::make_unique<qq::QQCommandHandler>(
          operations_, state_repository_, blocking_executor_)),
      event_handler_(std::make_unique<qq::QQEventHandler>(
          operations_, config_, state_repository_, received_message_repository_,
          blocking_executor_)),
      message_formatter_(std::make_unique<qq::QQMessageFormatter>(
          operations_, config_, state_repository_, blocking_executor_)) {
  if (!operations_) {
    throw std::invalid_argument("QQHandler requires bot operations");
  }
  message_formatter_->set_received_message_repository(
      received_message_repository_);
}

// 析构函数需要在这里定义，以确保所有子模块类的完整定义都可见
QQHandler::~QQHandler() = default;

auto QQHandler::forward_to_telegram(obcx::common::MessageEvent event)
    -> boost::asio::awaitable<DirectForwardOutcome> {
  DirectForwardOutcome outcome;
  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return outcome;
  }

  const std::string qq_group_id = event.group_id.value();
  const auto source_installation =
      operations_->onebot11_installation().installation_id;
  const auto target_installation =
      operations_->telegram_installation().installation_id;
  std::string telegram_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  auto [tg_id, topic_id] =
      config_->tg_group_and_topic_id(operations_->pair_id(), qq_group_id);
  OBCX_DEBUG("QQ群 {} 查找结果: TG群={}, topic_id={}", qq_group_id, tg_id,
             topic_id);

  if (tg_id.empty()) {
    OBCX_DEBUG("QQ群 {} 没有对应的Telegram群配置", qq_group_id);
    co_return outcome;
  }

  telegram_group_id = tg_id;
  const auto source_conversation_id = qq_conversation_id(qq_group_id);
  const auto target_conversation_id =
      telegram_conversation_id(telegram_group_id);
  bridge_config =
      config_->bridge_config(operations_->pair_id(), telegram_group_id);

  if (!bridge_config) {
    OBCX_DEBUG("无法找到Telegram群 {} 的配置", telegram_group_id);
    co_return outcome;
  }

  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    if (!bridge_config->enable_qq_to_tg) {
      OBCX_DEBUG("QQ群 {} 到Telegram群 {} 的转发已禁用，跳过", qq_group_id,
                 telegram_group_id);
      co_return outcome;
    }
  } else if (bridge_config->mode == BridgeMode::TOPIC_TO_GROUP) {
    const TopicBridgeConfig *topic_config = config_->topic_config(
        operations_->pair_id(), telegram_group_id, topic_id);
    if (!topic_config || !topic_config->enable_qq_to_tg) {
      OBCX_DEBUG("QQ群 {} 到Telegram topic {} 的转发已禁用，跳过", qq_group_id,
                 topic_id);
      co_return outcome;
    }
  }

  // 跳过从 Telegram 转发回来的回环消息
  if (event.raw_message.starts_with("[Telegram] ")) {
    OBCX_DEBUG("检测到可能是回环的Telegram消息，跳过转发");
    co_return outcome;
  }

  MessageMappingResolution existing;
  if (state_repository_) {
    existing = co_await blocking_executor_->run(
        [repository = state_repository_, source_installation,
         source_conversation_id, target_installation, target_conversation_id,
         message_id = event.message_id] {
          return repository->resolve_target_mapping(
              {.installation_id = source_installation,
               .platform = "qq",
               .conversation_id = source_conversation_id,
               .message_id = message_id},
              {.installation_id = target_installation,
               .platform = "telegram",
               .conversation_id = target_conversation_id},
              MessageMappingReadPurpose::PreSendDeduplication);
        });
  }
  if (existing.unique()) {
    OBCX_DEBUG("QQ消息 {} 已转发到Telegram，跳过重复处理", event.message_id);
    co_return DirectForwardOutcome{
        .disposition = DirectForwardDisposition::AlreadyPersisted,
        .source_platform = "qq",
        .source_bot = source_installation,
        .source_conversation_id = source_conversation_id,
        .source_message_id = event.message_id,
        .target_platform = "telegram",
        .target_bot = target_installation,
        .target_conversation_id = target_conversation_id,
        .target_message_id = existing.mapping->target_message_id,
    };
  }
  if (!existing.missing()) {
    co_return DirectForwardOutcome{
        .disposition = DirectForwardDisposition::DeliveryFailed,
        .source_platform = "qq",
        .source_bot = source_installation,
        .source_conversation_id = source_conversation_id,
        .source_message_id = event.message_id,
        .target_platform = "telegram",
        .target_bot = target_installation,
        .target_conversation_id = target_conversation_id,
        .failure_message = existing.diagnostic.empty()
                               ? "ambiguous_message_mapping"
                               : existing.diagnostic,
    };
  }

  OBCX_INFO("准备从QQ群 {} 转发消息到Telegram群 {}", qq_group_id,
            telegram_group_id);

  std::string fatal_error;
  try {
    obcx::common::Message message_to_send;

    std::string sender_display_name =
        co_await message_formatter_->format_sender_info(
            event, bridge_config, qq_group_id, telegram_group_id, topic_id,
            message_to_send);

    co_await message_formatter_->format_reply_message(event, message_to_send);

    std::vector<obcx::common::MessageSegment> image_segments;
    std::vector<obcx::common::MessageSegment> other_segments;

    for (const auto &segment : event.message) {
      if (segment.type == "reply") {
        continue; // reply 段已经在 format_reply_message 里消费过了
      }

      if (segment.type == "image") {
        image_segments.push_back(segment);
      } else {
        other_segments.push_back(segment);
      }
    }

    // 多张图片走 sendMediaGroup；单张或失败时退回到逐段发送（下方循环）。
    auto media_group = co_await message_formatter_->send_media_group(
        image_segments, other_segments, telegram_group_id, topic_id,
        sender_display_name, bridge_config, message_to_send, event);

    if (media_group.sent) {
      if (!media_group.primary_target_message_id.has_value()) {
        co_return DirectForwardOutcome{
            .disposition = DirectForwardDisposition::DeliveryFailed,
            .source_platform = "qq",
            .source_conversation_id = source_conversation_id,
            .source_message_id = event.message_id,
            .target_platform = "telegram",
            .target_conversation_id = target_conversation_id,
            .failure_message = "outcome_unknown",
        };
      }
      co_return DirectForwardOutcome{
          .disposition = DirectForwardDisposition::NewDelivery,
          .source_platform = "qq",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "telegram",
          .target_conversation_id = target_conversation_id,
          .target_message_id = *media_group.primary_target_message_id,
      };
    }

    std::vector<std::string> temp_files_to_cleanup;

    for (const auto &img_segment : image_segments) {
      auto converted_segment =
          co_await media_processor_->process_qq_media_segment(
              img_segment, event, telegram_group_id, topic_id,
              sender_display_name, bridge_config, temp_files_to_cleanup);

      if (converted_segment.has_value()) {
        message_to_send.push_back(converted_segment.value());
      }
    }

    for (const auto &segment : other_segments) {
      if (segment.type == "forward") {
        co_await message_formatter_->process_forward_message(
            segment, telegram_group_id, topic_id, message_to_send);
        continue;
      }

      if (segment.type == "node") {
        co_await message_formatter_->process_node_message(segment,
                                                          message_to_send);
        continue;
      }

      auto converted_segment =
          co_await media_processor_->process_qq_media_segment(
              segment, event, telegram_group_id, topic_id, sender_display_name,
              bridge_config, temp_files_to_cleanup);

      if (converted_segment.has_value()) {
        message_to_send.push_back(converted_segment.value());
      }
    }

    std::optional<std::string> telegram_message_id;
    std::string failure_reason;
    bool definitely_retryable = false;
    bool outcome_unknown = false;

    try {
      if (topic_id == -1) {
        telegram_message_id = co_await operations_->send_telegram_group(
            telegram_group_id, message_to_send);
      } else {
        telegram_message_id = co_await operations_->send_telegram_topic(
            telegram_group_id, topic_id, message_to_send);
      }
      outcome = DirectForwardOutcome{
          .disposition = DirectForwardDisposition::NewDelivery,
          .source_platform = "qq",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "telegram",
          .target_conversation_id = target_conversation_id,
          .target_message_id = *telegram_message_id,
      };
    } catch (const std::exception &error) {
      const auto disposition = classify_bridge_operation_failure(error);
      failure_reason = disposition.diagnostic;
      definitely_retryable = disposition.retryable;
      outcome_unknown = disposition.outcome_unknown;
      outcome = DirectForwardOutcome{
          .disposition = DirectForwardDisposition::DeliveryFailed,
          .source_platform = "qq",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "telegram",
          .target_conversation_id = target_conversation_id,
          .failure_message = failure_reason,
          .failure_retryable = definitely_retryable,
      };
      OBCX_WARN("发送QQ消息到Telegram时出错: {}", failure_reason);
    }

    if (!telegram_message_id.has_value() && definitely_retryable &&
        retry_manager_ && config_->enable_retry_queue) {
      OBCX_INFO("消息发送失败，添加到重试队列: {} -> {}", event.message_id,
                telegram_group_id);
      co_await blocking_executor_->run(
          [retry_manager = retry_manager_, source_installation,
           source_conversation_id, target_installation, target_conversation_id,
           message_id = event.message_id, message = message_to_send,
           telegram_group_id, qq_group_id, topic_id,
           max_attempts = config_->message_retry_max_attempts, failure_reason] {
            retry_manager->add_message_retry(
                source_installation, "qq", source_conversation_id,
                target_installation, "telegram", target_conversation_id,
                message_id, message, telegram_group_id, qq_group_id, topic_id,
                max_attempts, failure_reason);
          });
    } else if (!telegram_message_id.has_value() && outcome_unknown) {
      OBCX_ERROR("消息发送结果不确定，禁止自动重试: {}", failure_reason);
    } else if (!telegram_message_id.has_value() && definitely_retryable &&
               config_->enable_retry_queue) {
      OBCX_ERROR("消息发送失败且重试队列不可用: {}", failure_reason);
    } else if (!telegram_message_id.has_value()) {
      OBCX_ERROR("消息发送失败且未启用重试: {}", failure_reason);
    }

    if (!temp_files_to_cleanup.empty()) {
      co_await blocking_executor_->run(
          [files = std::move(temp_files_to_cleanup)] {
            for (const auto &file : files) {
              MediaProcessor::cleanup_media_file(file);
            }
          });
    }

  } catch (const std::exception &e) {
    OBCX_ERROR("转发QQ消息到Telegram时出错: {}", e.what());
    const auto mapping_error = std::string_view{e.what()}.find(
                                   "message_mapping") != std::string_view::npos;
    if (!mapping_error) {
      fatal_error = e.what();
    }
    outcome = DirectForwardOutcome{
        .disposition = DirectForwardDisposition::DeliveryFailed,
        .source_platform = "qq",
        .source_conversation_id = source_conversation_id,
        .source_message_id = event.message_id,
        .target_platform = "telegram",
        .target_conversation_id = target_conversation_id,
        .failure_message =
            mapping_error ? e.what() : "bridge_processing_failed",
    };
  }
  if (!fatal_error.empty()) {
    try {
      const obcx::common::Message error_message{
          {.type = "text",
           .data = {{"text",
                     fmt::format("转发消息到Telegram失败: {}", fatal_error)}}}};
      (void)co_await operations_->send_onebot11_group(qq_group_id,
                                                      error_message);
    } catch (const std::exception &error) {
      OBCX_WARN("发送QQ转发错误提示失败: {}", error.what());
    }
  }
  co_return outcome;
}

auto QQHandler::handle_recall_event(obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_recall_event(std::move(event));
}

auto QQHandler::handle_checkalive_command(obcx::common::MessageEvent event,
                                          const std::string &telegram_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_checkalive_command(std::move(event),
                                                       telegram_group_id);
}

auto QQHandler::handle_poke_event(const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_poke_event(event);
}

} // namespace bridge

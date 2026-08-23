#include "telegram/event_handler.hpp"

#include "bridge_state_repository.hpp"
#include "config.hpp"

#include <common/logger.hpp>
#include <utility>

namespace bridge::telegram {

TelegramEventHandler::TelegramEventHandler(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::function<boost::asio::awaitable<DirectForwardOutcome>(
        obcx::common::MessageEvent)>
        forward_function,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : operations_(std::move(operations)), config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)),
      forward_function_(std::move(forward_function)) {
  if (!operations_ || !config_) {
    throw std::invalid_argument(
        "TelegramEventHandler requires operations and configuration");
  }
}

auto TelegramEventHandler::handle_message_deleted(obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  (void)event;
  OBCX_DEBUG("Telegram消息删除事件处理尚未完全实现");
  co_return;
}

auto TelegramEventHandler::handle_message_edited(
    obcx::common::MessageEvent event)
    -> boost::asio::awaitable<DirectForwardOutcome> {
  DirectForwardOutcome outcome;
  const auto source_installation =
      operations_->telegram_installation().installation_id;
  const auto target_installation =
      operations_->onebot11_installation().installation_id;
  const auto source_message_id = event.message_id;
  std::string source_conversation;
  std::string target_conversation;
  try {
    if (event.message_type != "group" || !event.group_id.has_value()) {
      co_return outcome;
    }
    const auto telegram_group_id = *event.group_id;
    source_conversation = telegram_conversation_id(telegram_group_id);

    std::string qq_group_id;
    if (const auto *route =
            config_->bridge_config(operations_->pair_id(), telegram_group_id)) {
      if (route->mode == BridgeMode::GROUP_TO_GROUP) {
        qq_group_id = route->qq_group_id;
      } else {
        const auto topic_id =
            event.data.value("message_thread_id", std::int64_t{-1});
        qq_group_id = config_->qq_group_id_for_topic(
            operations_->pair_id(), telegram_group_id, topic_id);
      }
    }
    if (qq_group_id.empty()) {
      co_return outcome;
    }
    target_conversation = qq_conversation_id(qq_group_id);

    MessageMappingResolution mapping;
    if (state_repository_) {
      mapping = co_await blocking_executor_->run(
          [repository = state_repository_, source_installation,
           source_conversation, target_installation, target_conversation,
           source_message_id] {
            return repository->resolve_target_mapping(
                {.installation_id = source_installation,
                 .platform = "telegram",
                 .conversation_id = source_conversation,
                 .message_id = source_message_id},
                {.installation_id = target_installation,
                 .platform = "qq",
                 .conversation_id = target_conversation});
          });
    }
    if (mapping.missing()) {
      co_return outcome;
    }
    if (!mapping.unique()) {
      co_return DirectForwardOutcome{
          .disposition = DirectForwardDisposition::DeliveryFailed,
          .source_platform = "telegram",
          .source_conversation_id = source_conversation,
          .source_message_id = source_message_id,
          .target_platform = "qq",
          .target_conversation_id = target_conversation,
          .failure_message = mapping.diagnostic.empty()
                                 ? "ambiguous_message_mapping"
                                 : mapping.diagnostic};
    }

    bool recall_success = false;
    try {
      co_await operations_->delete_onebot11_message(
          qq_group_id, mapping.mapping->target_message_id);
      recall_success = true;
    } catch (const std::exception &error) {
      OBCX_WARN("QQ edit replacement delete failed: {}", error.what());
    }

    bool resend_failed = false;
    try {
      event.data["is_edited_resend"] = true;
      outcome = co_await forward_function_(std::move(event));
      resend_failed =
          outcome.disposition != DirectForwardDisposition::NewDelivery;
    } catch (const std::exception &error) {
      OBCX_ERROR("重发编辑后的消息时出错: {}", error.what());
      outcome = DirectForwardOutcome{
          .disposition = DirectForwardDisposition::DeliveryFailed,
          .source_platform = "telegram",
          .source_conversation_id = source_conversation,
          .source_message_id = source_message_id,
          .target_platform = "qq",
          .target_conversation_id = target_conversation,
          .failure_message = "bridge_edit_resend_failed",
      };
      resend_failed = true;
    }

    if (resend_failed && !recall_success && state_repository_) {
      (void)co_await blocking_executor_->run(
          [repository = state_repository_, source_installation,
           source_conversation, target_installation, target_conversation,
           source_message_id] {
            return repository->delete_message_mapping(
                {.installation_id = source_installation,
                 .platform = "telegram",
                 .conversation_id = source_conversation,
                 .message_id = source_message_id},
                {.installation_id = target_installation,
                 .platform = "qq",
                 .conversation_id = target_conversation});
          });
    }
  } catch (const std::exception &error) {
    OBCX_ERROR("处理Telegram编辑事件时出错: {}", error.what());
    outcome = DirectForwardOutcome{
        .disposition = DirectForwardDisposition::DeliveryFailed,
        .source_platform = "telegram",
        .source_conversation_id = source_conversation,
        .source_message_id = source_message_id,
        .target_platform = "qq",
        .target_conversation_id = target_conversation,
        .failure_message = "bridge_edit_failed",
    };
  }
  co_return outcome;
}

} // namespace bridge::telegram

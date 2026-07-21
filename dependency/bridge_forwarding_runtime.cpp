#include "bridge_forwarding_runtime.hpp"

#include "bridge_message_event_adapter.hpp"
#include "config.hpp"
#include "qq/handler.hpp"
#include "telegram/handler.hpp"

#include <stdexcept>
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

auto is_telegram_edit(const obcx::common::MessageEvent &event) -> bool {
  return event.sub_type == "edited" || (event.data.contains("is_edited") &&
                                        event.data["is_edited"].is_boolean() &&
                                        event.data["is_edited"].get<bool>());
}

} // namespace

BridgeForwardingRuntime::BridgeForwardingRuntime(
    std::shared_ptr<obcx::core::BotRegistry> bot_registry,
    std::shared_ptr<const BridgeConfig> config,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository,
    boost::asio::any_io_executor buffer_executor)
    : bot_registry_(std::move(bot_registry)), config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      qq_handler_(std::make_shared<QQHandler>(
          config_, nullptr, state_repository_, received_message_repository_)),
      telegram_handler_(std::make_shared<TelegramHandler>(
          config_, nullptr, std::move(buffer_executor), state_repository_,
          received_message_repository_)) {}

auto BridgeForwardingRuntime::forward_message(
    const obcx::core::MessageEnvelope &message)
    -> boost::asio::awaitable<BridgeForwardResult> {
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

} // namespace bridge

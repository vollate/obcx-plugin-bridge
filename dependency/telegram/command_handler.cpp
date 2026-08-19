#include "telegram/command_handler.hpp"

#include "bridge_state_repository.hpp"
#include "received_message_repository.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

#include <chrono>
#include <optional>
#include <utility>

namespace bridge::telegram {

TelegramCommandHandler::TelegramCommandHandler(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<bridge::ReceivedMessageRepository>
        received_message_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : operations_(std::move(operations)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      blocking_executor_(std::move(blocking_executor)) {
  if (!operations_) {
    throw std::invalid_argument(
        "TelegramCommandHandler requires bot operations");
  }
}

auto TelegramCommandHandler::handle_recall_command(
    obcx::common::MessageEvent event, const std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  try {
    const auto telegram_group_id = event.group_id.value();
    if (!event.data.contains("reply_to_message") ||
        !event.data["reply_to_message"].contains("message_id")) {
      co_await send_reply_message(
          telegram_group_id, event.message_id,
          "⚠️ 请回复一条消息后使用 /recall 命令来撤回对应的QQ消息");
      co_return;
    }
    const auto replied_message_id = std::to_string(
        event.data["reply_to_message"]["message_id"].get<std::int64_t>());
    std::optional<std::string> target_message_id;
    if (state_repository_) {
      target_message_id = co_await blocking_executor_->run(
          [repository = state_repository_, replied_message_id] {
            auto target = repository->get_target_message_id(
                "telegram", replied_message_id, "qq");
            return target.has_value()
                       ? target
                       : repository->get_source_message_id(
                             "telegram", replied_message_id, "qq");
          });
    }
    if (!target_message_id.has_value()) {
      co_await send_reply_message(
          telegram_group_id, event.message_id,
          "❌ 未找到该消息对应的QQ消息，可能不是转发消息或已过期");
      co_return;
    }

    std::string reply;
    try {
      co_await operations_->delete_onebot11_message(qq_group_id,
                                                    *target_message_id);
      reply = "✅ 撤回成功";
      if (state_repository_) {
        (void)co_await blocking_executor_->run([repository = state_repository_,
                                                replied_message_id] {
          return repository->delete_message_mapping("telegram",
                                                    replied_message_id, "qq");
        });
      }
    } catch (const std::exception &error) {
      OBCX_WARN("/recall typed QQ delete failed: {}", error.what());
      reply = "❌ 撤回操作失败，请稍后重试";
    }
    co_await send_reply_message(telegram_group_id, event.message_id, reply);
  } catch (const std::exception &error) {
    OBCX_ERROR("处理 /recall 命令时出错: {}", error.what());
  }
}

auto TelegramCommandHandler::send_reply_message(
    const std::string &telegram_group_id,
    const std::string &reply_to_message_id, const std::string &text)
    -> boost::asio::awaitable<void> {
  obcx::common::Message message{
      {.type = "reply", .data = {{"id", reply_to_message_id}}},
      {.type = "text", .data = {{"text", text}}},
  };
  (void)co_await operations_->send_telegram_group(telegram_group_id, message);
}

auto TelegramCommandHandler::handle_checkalive_command(
    obcx::common::MessageEvent event, const std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  (void)qq_group_id;
  try {
    const auto telegram_group_id = event.group_id.value();
    std::optional<storage::PlatformHeartbeatInfo> qq_heartbeat;
    std::optional<storage::PlatformHeartbeatInfo> telegram_heartbeat;
    if (state_repository_) {
      std::tie(qq_heartbeat, telegram_heartbeat) =
          co_await blocking_executor_->run([repository = state_repository_] {
            return std::pair{repository->get_platform_heartbeat("qq"),
                             repository->get_platform_heartbeat("telegram")};
          });
    }

    const auto now = std::chrono::system_clock::now();
    std::string response;
    if (qq_heartbeat.has_value()) {
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - qq_heartbeat->last_heartbeat_at)
                           .count();
      response += fmt::format("🤖 QQ平台状态:\n最后心跳: {} 秒前\n{}\n", age,
                              age > 60 ? "⚠️ QQ平台可能离线" : "✅ QQ平台正常");
    } else {
      response += "🤖 QQ平台状态: ❌ 无心跳记录\n";
    }
    if (telegram_heartbeat.has_value()) {
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           now - telegram_heartbeat->last_heartbeat_at)
                           .count();
      response += fmt::format(
          "\n💬 Telegram平台状态:\n最后活动: {} 秒前\n{}", age,
          age > 40 ? "⚠️ Telegram平台可能离线" : "✅ Telegram平台正常");
    } else {
      response += "\n💬 Telegram平台状态: ❌ 无活动记录";
    }
    co_await send_reply_message(telegram_group_id, event.message_id, response);
  } catch (const std::exception &error) {
    OBCX_ERROR("处理 /checkalive 命令时出错: {}", error.what());
  }
}

auto TelegramCommandHandler::handle_poke_command(
    obcx::common::MessageEvent event, const std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  try {
    const auto telegram_group_id = event.group_id.value();
    if (!event.data.contains("reply_to_message") ||
        !event.data["reply_to_message"].contains("message_id")) {
      co_await send_reply_message(
          telegram_group_id, event.message_id,
          "⚠️ 请回复一条消息后使用 /poke 命令来戳对应的QQ用户");
      co_return;
    }
    const auto replied_message_id = std::to_string(
        event.data["reply_to_message"]["message_id"].get<std::int64_t>());
    std::string target_user_id;
    if (state_repository_ || received_message_repository_) {
      target_user_id = co_await blocking_executor_->run(
          [state = state_repository_, received = received_message_repository_,
           replied_message_id] {
            if (received) {
              if (auto source =
                      received->get_message("telegram", replied_message_id)) {
                return source->user_id;
              }
            }
            if (!state || !received) {
              return std::string{};
            }
            auto id = state->get_target_message_id("telegram",
                                                   replied_message_id, "qq");
            if (id.has_value()) {
              if (auto message = received->get_message("qq", *id)) {
                return message->user_id;
              }
            }
            id = state->get_source_message_id("telegram", replied_message_id,
                                              "qq");
            if (id.has_value()) {
              if (auto message = received->get_message("qq", *id)) {
                return message->user_id;
              }
            }
            return std::string{};
          });
    }
    if (target_user_id.empty()) {
      co_await send_reply_message(
          telegram_group_id, event.message_id,
          "❌ 未找到该消息对应的QQ用户，可能不是从QQ转发的消息或已过期");
      co_return;
    }
    bool poke_failed = false;
    try {
      co_await operations_->poke_onebot11_group(qq_group_id, target_user_id);
    } catch (const std::exception &error) {
      OBCX_WARN("/poke typed OneBot operation failed: {}", error.what());
      poke_failed = true;
    }
    if (poke_failed) {
      co_await send_reply_message(telegram_group_id, event.message_id,
                                  "❌ 戳一戳操作失败，请稍后重试");
    }
  } catch (const std::exception &error) {
    OBCX_ERROR("处理 /poke 命令时出错: {}", error.what());
  }
}

} // namespace bridge::telegram

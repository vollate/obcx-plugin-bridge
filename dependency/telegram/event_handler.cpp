#include "telegram/event_handler.hpp"
#include "bridge_state_repository.hpp"

#include <common/logger.hpp>
#include <nlohmann/json.hpp>
#include <utility>

namespace bridge::telegram {

TelegramEventHandler::TelegramEventHandler(
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::function<boost::asio::awaitable<void>(
        obcx::core::IBot &, obcx::core::IBot &, obcx::common::MessageEvent)>
        forward_function,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)),
      forward_function_(std::move(forward_function)) {}

auto TelegramEventHandler::handle_message_deleted(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::Event event) -> boost::asio::awaitable<void> {

  try {
    // Telegram的删除事件目前未实现：需要先了解 adapter 暴露的删除事件格式
    // （例如可能在 context 中通过 "deleted": true 标记）
    OBCX_DEBUG("Telegram消息删除事件处理尚未完全实现");
    co_return;

  } catch (const std::exception &e) {
    OBCX_ERROR("处理Telegram删除事件时出错: {}", e.what());
  }
}

auto TelegramEventHandler::handle_message_edited(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event) -> boost::asio::awaitable<void> {

  try {
    if (event.message_type != "group" || !event.group_id.has_value()) {
      co_return;
    }

    const std::string telegram_group_id = event.group_id.value();
    OBCX_INFO("处理Telegram群 {} 中消息 {} 的编辑事件", telegram_group_id,
              event.message_id);

    std::optional<std::string> target_message_id;
    if (state_repository_) {
      target_message_id = co_await blocking_executor_->run(
          [repository = state_repository_, message_id = event.message_id] {
            return repository->get_target_message_id("telegram", message_id,
                                                     "qq");
          });
    }

    if (!target_message_id.has_value()) {
      OBCX_DEBUG("未找到Telegram消息 {} 对应的QQ消息映射", event.message_id);
      co_return;
    }

    bool recall_success = false;

    try {
      // 编辑流程：先撤回旧的QQ消息，再重发编辑后的内容
      auto recall_response =
          co_await qq_bot.delete_message(target_message_id.value());

      nlohmann::json recall_json = nlohmann::json::parse(recall_response);

      if (recall_json.contains("status") && recall_json["status"] == "ok") {
        OBCX_INFO("成功在QQ撤回消息: {}", target_message_id.value());
        recall_success = true;
      } else {
        OBCX_WARN("QQ撤回消息失败: {}, 响应: {}", target_message_id.value(),
                  recall_response);
      }

    } catch (const std::exception &e) {
      OBCX_WARN("尝试在QQ撤回消息时出错: {}", e.what());
    }

    // 无论撤回是否成功，都尝试重发编辑后的消息
    OBCX_INFO("开始重发编辑后的消息到QQ (撤回状态: {})",
              recall_success ? "成功" : "失败");

    bool resend_failed = false;
    try {
      // 标记此消息为编辑消息，让 forward_function_ 走"更新映射"分支而非新建
      const_cast<nlohmann::json &>(event.data)["is_edited_resend"] = true;

      co_await forward_function_(telegram_bot, qq_bot, event);

      OBCX_INFO("成功重发编辑后的消息");
    } catch (const std::exception &e) {
      OBCX_ERROR("重发编辑后的消息时出错: {}", e.what());
      resend_failed = true;
    }

    if (resend_failed) {
      if (recall_success) {
        OBCX_WARN("撤回成功但重发失败，原QQ消息已被撤回但新消息发送失败");
      } else {
        // 撤回失败且重发也失败时，删除映射避免数据不一致
        if (state_repository_) {
          (void)co_await blocking_executor_->run(
              [repository = state_repository_, message_id = event.message_id] {
                return repository->delete_message_mapping("telegram",
                                                          message_id, "qq");
              });
        }
        OBCX_WARN("撤回和重发都失败，已删除消息映射");
      }
      co_return;
    }

  } catch (const std::exception &e) {
    OBCX_ERROR("处理Telegram编辑事件时出错: {}", e.what());
  }
}

} // namespace bridge::telegram

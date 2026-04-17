#include "telegram/event_handler.hpp"

#include <common/logger.hpp>
#include <nlohmann/json.hpp>
#include <utility>

namespace bridge::telegram {

TelegramEventHandler::TelegramEventHandler(
    std::shared_ptr<storage::DatabaseManager> db_manager,
    std::function<boost::asio::awaitable<void>(
        obcx::core::IBot &, obcx::core::IBot &, obcx::common::MessageEvent)>
        forward_function)
    : db_manager_(std::move(db_manager)),
      forward_function_(std::move(forward_function)) {}

auto TelegramEventHandler::handle_message_deleted(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::Event event) -> boost::asio::awaitable<void> {

  try {
    // Telegram的删除事件通常也是MessageEvent，但需要特殊的标识
    // 这里假设删除事件会在context中包含"deleted": true标识
    // 具体实现需要根据Telegram adapter的事件格式调整

    // 暂时跳过，因为需要先了解Telegram删除事件的具体格式
    PLUGIN_DEBUG("tg_to_qq", "Telegram消息删除事件处理尚未完全实现");
    co_return;

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理Telegram删除事件时出错: {}", e.what());
  }
}

auto TelegramEventHandler::handle_message_edited(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event) -> boost::asio::awaitable<void> {

  try {
    // 确保是群消息编辑
    if (event.message_type != "group" || !event.group_id.has_value()) {
      co_return;
    }

    const std::string telegram_group_id = event.group_id.value();
    PLUGIN_INFO("tg_to_qq", "处理Telegram群 {} 中消息 {} 的编辑事件",
                telegram_group_id, event.message_id);

    // 查找对应的QQ消息ID
    auto target_message_id =
        db_manager_->get_target_message_id("telegram", event.message_id, "qq");

    if (!target_message_id.has_value()) {
      PLUGIN_DEBUG("tg_to_qq", "未找到Telegram消息 {} 对应的QQ消息映射",
                   event.message_id);
      co_return;
    }

    bool recall_success = false;

    try {
      // 先尝试撤回QQ上的原消息
      auto recall_response =
          co_await qq_bot.delete_message(target_message_id.value());

      // 解析撤回响应
      nlohmann::json recall_json = nlohmann::json::parse(recall_response);

      if (recall_json.contains("status") && recall_json["status"] == "ok") {
        PLUGIN_INFO("tg_to_qq", "成功在QQ撤回消息: {}",
                    target_message_id.value());
        recall_success = true;
      } else {
        PLUGIN_WARN("tg_to_qq", "QQ撤回消息失败: {}, 响应: {}",
                    target_message_id.value(), recall_response);
      }

    } catch (const std::exception &e) {
      PLUGIN_WARN("tg_to_qq", "尝试在QQ撤回消息时出错: {}", e.what());
    }

    // 无论撤回是否成功，都尝试重发编辑后的消息
    PLUGIN_INFO("tg_to_qq", "开始重发编辑后的消息到QQ (撤回状态: {})",
                recall_success ? "成功" : "失败");

    try {
      // 标记此消息为编辑消息，以便forward_function_可以识别并更新映射而非创建新映射
      // 通过在event.data中添加标记
      const_cast<nlohmann::json &>(event.data)["is_edited_resend"] = true;

      // 使用传入的转发函数重发编辑后的内容
      co_await forward_function_(telegram_bot, qq_bot, event);

      PLUGIN_INFO("tg_to_qq", "成功重发编辑后的消息");

    } catch (const std::exception &e) {
      PLUGIN_ERROR("tg_to_qq", "重发编辑后的消息时出错: {}", e.what());

      // 如果是撤回成功但重发失败的情况，需要恢复映射或处理
      if (recall_success) {
        PLUGIN_WARN("tg_to_qq",
                    "撤回成功但重发失败，原QQ消息已被撤回但新消息发送失败");
      } else {
        // 撤回失败且重发也失败时，删除映射避免数据不一致
        db_manager_->delete_message_mapping("telegram", event.message_id, "qq");
        PLUGIN_WARN("tg_to_qq", "撤回和重发都失败，已删除消息映射");
      }
      co_return;
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理Telegram编辑事件时出错: {}", e.what());
  }
}

} // namespace bridge::telegram

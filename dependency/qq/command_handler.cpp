#include "qq/command_handler.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::qq {

QQCommandHandler::QQCommandHandler(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto QQCommandHandler::handle_checkalive_command(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event, const std::string &telegram_group_id)
    -> boost::asio::awaitable<void> {

  try {
    const std::string qq_group_id = event.group_id.value();

    // 获取QQ平台的心跳信息
    auto qq_heartbeat = db_manager_->get_platform_heartbeat("qq");
    // 获取Telegram平台的心跳信息
    auto telegram_heartbeat = db_manager_->get_platform_heartbeat("telegram");

    std::string response_text;

    if (qq_heartbeat.has_value()) {
      auto qq_time_point = qq_heartbeat->last_heartbeat_at;
      auto qq_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                              qq_time_point.time_since_epoch())
                              .count();

      // 计算距离现在的时间差
      auto now = std::chrono::system_clock::now();
      auto qq_duration =
          std::chrono::duration_cast<std::chrono::seconds>(now - qq_time_point)
              .count();

      response_text += fmt::format("🤖 QQ平台状态:\n");
      response_text +=
          fmt::format("最后心跳: {} ({} 秒前)\n", qq_timestamp, qq_duration);

      if (qq_duration > 60) {
        response_text += "⚠️ QQ平台可能离线\n";
      } else {
        response_text += "✅ QQ平台正常\n";
      }
    } else {
      response_text += "🤖 QQ平台状态: ❌ 无心跳记录\n";
    }

    response_text += "\n";

    if (telegram_heartbeat.has_value()) {
      auto tg_time_point = telegram_heartbeat->last_heartbeat_at;
      auto tg_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                              tg_time_point.time_since_epoch())
                              .count();

      // 计算距离现在的时间差
      auto now = std::chrono::system_clock::now();
      auto tg_duration =
          std::chrono::duration_cast<std::chrono::seconds>(now - tg_time_point)
              .count();

      response_text += fmt::format("💬 Telegram平台状态:\n");
      response_text +=
          fmt::format("最后活动: {} ({} 秒前)\n", tg_timestamp, tg_duration);

      if (tg_duration > 300) {
        // 5分钟无活动认为异常
        response_text += "⚠️ Telegram平台可能离线";
      } else {
        response_text += "✅ Telegram平台正常";
      }
    } else {
      response_text += "💬 Telegram平台状态: ❌ 无活动记录";
    }

    // 发送回复消息
    co_await send_reply_message(qq_bot, qq_group_id, event.message_id,
                                response_text);

    PLUGIN_INFO("qq_to_tg", "/checkalive 命令处理完成");

  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理 /checkalive 命令时出错: {}", e.what());
  }
}

auto QQCommandHandler::send_reply_message(
    obcx::core::IBot &qq_bot, const std::string &qq_group_id,
    const std::string &reply_to_message_id, const std::string &text)
    -> boost::asio::awaitable<void> {
  try {
    // 构造回复消息
    obcx::common::Message reply_message;

    // 添加回复segment
    obcx::common::MessageSegment reply_segment;
    reply_segment.type = "reply";
    reply_segment.data["id"] = reply_to_message_id;
    reply_message.push_back(reply_segment);

    obcx::common::MessageSegment text_segment;
    text_segment.type = "text";
    text_segment.data["text"] = text;
    reply_message.push_back(text_segment);

    // 发送到QQ
    co_await qq_bot.send_group_message(qq_group_id, reply_message);

  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "发送回复消息失败: {}", e.what());
  }
}

} // namespace bridge::qq

#include "telegram/command_handler.hpp"

#include <common/logger.hpp>
#include <core/qq_bot.hpp>

#include <nlohmann/json.hpp>
#include <utility>

namespace bridge::telegram {

TelegramCommandHandler::TelegramCommandHandler(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto TelegramCommandHandler::handle_recall_command(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event, std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {

  try {
    const std::string telegram_group_id = event.group_id.value();

    // 检查是否回复了消息
    if (!event.data.contains("reply_to_message")) {
      // 没有回复消息，发送使用说明
      co_await send_reply_message(
          telegram_bot, telegram_group_id, event.message_id,
          "⚠️ 请回复一条消息后使用 /recall 命令来撤回对应的QQ消息");
      co_return;
    }

    // 获取被回复的消息ID
    auto reply_to_message = event.data["reply_to_message"];
    if (!reply_to_message.contains("message_id")) {
      PLUGIN_WARN("tg_to_qq", "/recall 命令：无法获取被回复消息的ID");
      co_return;
    }

    std::string replied_message_id =
        std::to_string(reply_to_message["message_id"].get<int64_t>());
    PLUGIN_INFO("tg_to_qq",
                "/recall 命令：尝试撤回回复的Telegram消息 {} 对应的QQ消息",
                replied_message_id);

    // 查找被回复消息对应的QQ消息ID
    std::optional<std::string> target_qq_message_id;

    // 首先查找该Telegram消息是否已被转发到QQ（回复的是已转发的消息）
    target_qq_message_id = db_manager_->get_target_message_id(
        "telegram", replied_message_id, "qq");

    // 如果没找到，再查找该消息是否来源于QQ（回复的是从QQ转发过来的消息）
    if (!target_qq_message_id.has_value()) {
      target_qq_message_id = db_manager_->get_source_message_id(
          "telegram", replied_message_id, "qq");
    }

    if (!target_qq_message_id.has_value()) {
      // 没有找到对应的QQ消息
      co_await send_reply_message(
          telegram_bot, telegram_group_id, event.message_id,
          "❌ 未找到该消息对应的QQ消息，可能不是转发消息或已过期");
      co_return;
    }

    PLUGIN_INFO("tg_to_qq", "/recall 命令：找到对应的QQ消息ID: {}",
                target_qq_message_id.value());

    // 尝试在QQ撤回消息
    std::string result_message;
    try {
      std::string recall_response =
          co_await qq_bot.delete_message(target_qq_message_id.value());

      // 解析QQ撤回响应
      nlohmann::json recall_json = nlohmann::json::parse(recall_response);

      if (recall_json.contains("status") && recall_json["status"] == "ok") {
        result_message = "✅ 撤回成功";
        PLUGIN_INFO("tg_to_qq", "/recall 命令：成功在QQ撤回消息 {}",
                    target_qq_message_id.value());

        // 撤回成功，删除数据库映射
        db_manager_->delete_message_mapping("telegram", replied_message_id,
                                            "qq");
        PLUGIN_DEBUG("tg_to_qq", "已删除消息映射: telegram:{} -> qq:{}",
                     replied_message_id, target_qq_message_id.value());

      } else {
        result_message = "❌ 撤回失败";
        if (recall_json.contains("message")) {
          std::string error_msg = recall_json["message"];
          if (error_msg.find("超时") != std::string::npos ||
              error_msg.find("timeout") != std::string::npos ||
              error_msg.find("时间") != std::string::npos) {
            result_message += "：消息发送时间过久，无法撤回";
          } else if (error_msg.find("权限") != std::string::npos ||
                     error_msg.find("permission") != std::string::npos) {
            result_message += "：没有撤回权限";
          } else {
            result_message += "：" + error_msg;
          }
        }
        PLUGIN_WARN("tg_to_qq", "/recall 命令：QQ撤回消息失败: {}",
                    recall_response);
      }

    } catch (const std::exception &e) {
      PLUGIN_ERROR("tg_to_qq", "/recall 命令：QQ撤回操作异常: {}", e.what());
      result_message = "❌ 撤回操作发生异常，请稍后重试";
    }

    // 发送撤回结果消息
    try {
      co_await send_reply_message(telegram_bot, telegram_group_id,
                                  event.message_id, result_message);
    } catch (const std::exception &send_e) {
      PLUGIN_ERROR("tg_to_qq", "/recall 命令：发送结果消息失败: {}",
                   send_e.what());
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理 /recall 命令时出错: {}", e.what());
  }
}

auto TelegramCommandHandler::send_reply_message(
    obcx::core::IBot &telegram_bot, const std::string &telegram_group_id,
    const std::string &reply_to_message_id, const std::string &text)
    -> boost::asio::awaitable<void> {

  try {
    obcx::common::Message message;

    obcx::common::MessageSegment reply_segment;
    reply_segment.type = "reply";
    reply_segment.data["id"] = reply_to_message_id;
    message.push_back(reply_segment);

    obcx::common::MessageSegment text_segment;
    text_segment.type = "text";
    text_segment.data["text"] = text;
    message.push_back(text_segment);

    co_await telegram_bot.send_group_message(telegram_group_id, message);
  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "发送回复消息失败: {}", e.what());
  }
}

auto TelegramCommandHandler::handle_checkalive_command(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event, std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {

  try {
    const std::string telegram_group_id = event.group_id.value();

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

      if (tg_duration > 40) { // 5分钟无活动认为异常
        response_text += "⚠️ Telegram平台可能离线";
      } else {
        response_text += "✅ Telegram平台正常";
      }
    } else {
      response_text += "💬 Telegram平台状态: ❌ 无活动记录";
    }

    // 发送回复消息
    co_await send_reply_message(telegram_bot, telegram_group_id,
                                event.message_id, response_text);

    PLUGIN_INFO("tg_to_qq", "/checkalive 命令处理完成");

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理 /checkalive 命令时出错: {}", e.what());

    // 发送错误消息 - 使用简单的错误处理，不使用co_await在catch块中
    // 这里记录错误但不发送消息，因为co_await不能在catch块中使用
  }
}

auto TelegramCommandHandler::handle_poke_command(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    obcx::common::MessageEvent event, std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {

  try {
    const std::string telegram_group_id = event.group_id.value();

    // 检查是否回复了消息
    if (!event.data.contains("reply_to_message")) {
      // 没有回复消息，发送使用说明
      co_await send_reply_message(
          telegram_bot, telegram_group_id, event.message_id,
          "⚠️ 请回复一条消息后使用 /poke 命令来戳对应的QQ用户");
      co_return;
    }

    // 获取被回复的消息ID
    auto reply_to_message = event.data["reply_to_message"];
    if (!reply_to_message.contains("message_id")) {
      PLUGIN_WARN("tg_to_qq", "/poke 命令：无法获取被回复消息的ID");
      co_return;
    }

    std::string replied_message_id =
        std::to_string(reply_to_message["message_id"].get<int64_t>());
    PLUGIN_INFO("tg_to_qq",
                "/poke 命令：尝试戳回复的Telegram消息 {} 对应的QQ用户",
                replied_message_id);

    // 查找被回复消息对应的QQ用户ID
    // 首先检查这条消息是否来源于QQ（从QQ转发过来的消息）
    auto source_message =
        db_manager_->get_message("telegram", replied_message_id);

    std::string target_qq_user_id;

    if (source_message.has_value()) {
      // 如果消息记录存在，获取发送者的QQ用户ID
      // 这条消息是从QQ转发过来的
      target_qq_user_id = source_message->user_id;
      PLUGIN_INFO("tg_to_qq", "/poke 命令：从消息记录找到QQ用户ID: {}",
                  target_qq_user_id);
    } else {
      // 消息记录不存在，尝试通过消息映射查找
      // 先查找该消息是否已转发到QQ
      auto target_qq_message_id = db_manager_->get_target_message_id(
          "telegram", replied_message_id, "qq");

      if (target_qq_message_id.has_value()) {
        // 找到了对应的QQ消息，尝试获取该消息的发送者
        auto qq_message =
            db_manager_->get_message("qq", target_qq_message_id.value());
        if (qq_message.has_value()) {
          target_qq_user_id = qq_message->user_id;
          PLUGIN_INFO("tg_to_qq",
                      "/poke 命令：从转发的QQ消息记录找到QQ用户ID: {}",
                      target_qq_user_id);
        }
      }

      // 如果还是没找到，查找该消息是否来源于QQ
      if (target_qq_user_id.empty()) {
        auto source_qq_message_id = db_manager_->get_source_message_id(
            "telegram", replied_message_id, "qq");

        if (source_qq_message_id.has_value()) {
          auto qq_message =
              db_manager_->get_message("qq", source_qq_message_id.value());
          if (qq_message.has_value()) {
            target_qq_user_id = qq_message->user_id;
            PLUGIN_INFO("tg_to_qq",
                        "/poke 命令：从源QQ消息记录找到QQ用户ID: {}",
                        target_qq_user_id);
          }
        }
      }
    }

    if (target_qq_user_id.empty()) {
      // 没有找到对应的QQ用户
      co_await send_reply_message(
          telegram_bot, telegram_group_id, event.message_id,
          "❌ 未找到该消息对应的QQ用户，可能不是从QQ转发的消息或已过期");
      co_return;
    }

    PLUGIN_INFO("tg_to_qq", "/poke 命令：准备戳QQ群 {} 的用户 {}",
                std::string(qq_group_id), target_qq_user_id);

    // 获取QQBot实例并调用group_poke
    auto *qq_bot_ptr = dynamic_cast<obcx::core::QQBot *>(&qq_bot);
    if (!qq_bot_ptr) {
      PLUGIN_ERROR("tg_to_qq", "/poke 命令：无法获取QQBot实例");
      co_await send_reply_message(telegram_bot, telegram_group_id,
                                  event.message_id,
                                  "❌ 内部错误：无法获取QQ机器人实例");
      co_return;
    }

    // 调用戳一戳API
    std::string result_message;
    try {
      std::string poke_response =
          co_await qq_bot_ptr->group_poke(qq_group_id, target_qq_user_id);

      // 解析QQ戳一戳响应
      nlohmann::json poke_json = nlohmann::json::parse(poke_response);

      if (poke_json.contains("status") && poke_json["status"] == "ok") {
        // result_message = "✅ 已成功戳了对方";
        PLUGIN_INFO("tg_to_qq", "/poke 命令：成功在QQ群 {} 戳了用户 {}",
                    std::string(qq_group_id), target_qq_user_id);
        co_return;
      } else {
        result_message = "❌ 戳一戳失败";
        if (poke_json.contains("message")) {
          std::string error_msg = poke_json["message"];
          result_message += "：" + error_msg;
        }
        PLUGIN_WARN("tg_to_qq", "/poke 命令：戳一戳失败: {}", poke_response);
      }

    } catch (const std::exception &e) {
      PLUGIN_ERROR("tg_to_qq", "/poke 命令：戳一戳操作异常: {}", e.what());
      result_message = "❌ 戳一戳操作发生异常，请稍后重试";
    }

    // 发送结果消息
    try {
      co_await send_reply_message(telegram_bot, telegram_group_id,
                                  event.message_id, result_message);
    } catch (const std::exception &send_e) {
      PLUGIN_ERROR("tg_to_qq", "/poke 命令：发送结果消息失败: {}",
                   send_e.what());
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理 /poke 命令时出错: {}", e.what());
  }
}

} // namespace bridge::telegram

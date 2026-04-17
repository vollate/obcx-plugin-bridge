#include "telegram/message_formatter.hpp"
#include "config.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::telegram {

auto TelegramMessageFormatter::format_sender_info(
    const obcx::common::MessageEvent &event,
    const GroupBridgeConfig *bridge_config,
    const std::string &telegram_group_id,
    std::vector<obcx::common::MessageSegment> &message_to_send) -> void {

  // 根据配置决定是否添加发送者信息
  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_tg_to_qq_sender;
  } else {
    // Topic模式：获取对应topic的配置
    int64_t message_thread_id = -1;
    if (event.data.contains("message_thread_id")) {
      message_thread_id = event.data["message_thread_id"].get<int64_t>();
    }
    const TopicBridgeConfig *topic_config =
        get_topic_config(telegram_group_id, message_thread_id);
    show_sender = topic_config ? topic_config->show_tg_to_qq_sender : false;
  }

  if (show_sender) {
    std::string sender_display_name = get_sender_display_name(event);
    std::string sender_info = fmt::format("[{}]\t", sender_display_name);

    // 添加发送者信息作为文本段
    obcx::common::MessageSegment sender_segment;
    sender_segment.type = "text";

    // 如果没有找到对应的回复消息ID，在发送者信息中加上回复提示
    bool has_reply = has_reply_segment(message_to_send);
    bool has_genuine_reply = is_genuine_reply(event);

    sender_segment.data["text"] =
        has_reply ? sender_info
                  : (has_genuine_reply ? sender_info + "[回复一条消息] "
                                       : sender_info);
    message_to_send.push_back(sender_segment);
    PLUGIN_DEBUG("tg_to_qq", "Telegram到QQ转发显示发送者：{}",
                 sender_display_name);
  } else {
    // 不显示发送者，但如果有回复需要添加提示
    if (event.data.contains("reply_to_message")) {
      bool has_reply = has_reply_segment(message_to_send);
      bool has_genuine_reply = is_genuine_reply(event);

      if (!has_reply && has_genuine_reply) {
        obcx::common::MessageSegment reply_tip_segment;
        reply_tip_segment.type = "text";
        reply_tip_segment.data["text"] = "[回复一条消息] ";
        message_to_send.push_back(reply_tip_segment);
      }
    }
    PLUGIN_DEBUG("tg_to_qq", "Telegram到QQ转发不显示发送者");
  }
}

auto TelegramMessageFormatter::format_reply_message(
    const obcx::common::MessageEvent &event,
    const std::optional<std::string> &reply_to_message_id,
    std::vector<obcx::common::MessageSegment> &message_to_send) -> void {

  if (event.data.contains("reply_to_message") &&
      reply_to_message_id.has_value()) {
    // 创建回复消息段
    obcx::common::MessageSegment reply_segment;
    reply_segment.type = "reply";
    reply_segment.data["id"] = reply_to_message_id.value();
    message_to_send.insert(message_to_send.begin(), reply_segment);

    PLUGIN_DEBUG("tg_to_qq", "Telegram消息回复QQ消息: {} -> QQ消息ID: {}",
                 event.message_id, reply_to_message_id.value());
  } else if (event.data.contains("reply_to_message")) {
    PLUGIN_DEBUG(
        "tg_to_qq",
        "未找到Telegram引用消息对应的QQ消息ID，可能是原生Telegram消息");
  }
}

auto TelegramMessageFormatter::get_sender_display_name(
    const obcx::common::MessageEvent &event) -> std::string {

  std::string sender_display_name = "Unknown";

  if (event.data.contains("from")) {
    auto from = event.data["from"];
    if (from.contains("first_name")) {
      sender_display_name = from["first_name"].get<std::string>();
      if (from.contains("last_name")) {
        sender_display_name += " " + from["last_name"].get<std::string>();
      }
    } else if (from.contains("username")) {
      sender_display_name = "@" + from["username"].get<std::string>();
    }
  }

  return sender_display_name;
}

auto TelegramMessageFormatter::is_genuine_reply(
    const obcx::common::MessageEvent &event) -> bool {

  if (!event.data.contains("reply_to_message")) {
    return false;
  }

  // 【重要】Telegram Topic消息特殊处理逻辑：
  // 在Telegram Group的Topic中，消息可能包含reply_to_message但不是真正的回复
  if (event.data.contains("message_thread_id")) {
    auto reply_to_message = event.data["reply_to_message"];
    int64_t thread_id = event.data["message_thread_id"].get<int64_t>();
    int64_t reply_msg_id = reply_to_message["message_id"].get<int64_t>();

    // 关键逻辑：
    // - 如果 reply_msg_id == thread_id，说明是回复给Topic的根消息（创建消息），
    //   这实际上是在Topic中发送消息，不是真正的回复
    // - 如果 reply_msg_id != thread_id，说明是回复Topic中其他用户的消息，
    //   这才是真正的回复
    return (reply_msg_id != thread_id);
  }

  // 非Topic消息的回复都是真回复
  return true;
}

auto TelegramMessageFormatter::has_reply_segment(
    const std::vector<obcx::common::MessageSegment> &message_to_send) -> bool {

  return !message_to_send.empty() && message_to_send[0].type == "reply";
}

} // namespace bridge::telegram

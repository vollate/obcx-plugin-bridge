// QQ媒体处理器：@消息段。
//
// 将 QQ 的 [CQ:at,qq=xxxxx] 转换为 Telegram 中可读的 @用户 文本。
// 显示规则取决于桥接配置（是否显示发送者；topic 模式与 group 模式分别取配置）。

#include "qq/media_processor.hpp"
#include "qq/message_formatter.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::qq {

auto QQMediaProcessor::process_at_segment(
    obcx::core::IBot &qq_bot, const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const GroupBridgeConfig *bridge_config)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment;
  converted_segment.type = "text";
  std::string qq_user_id = segment.data.value("qq", "unknown");
  converted_segment.data.clear();

  // 从数据库查询用户的显示名称（使用群组特定的昵称）
  auto at_display_name = db_manager_->query_user_display_name(
      "qq", qq_user_id, event.group_id.value_or(""));

  // 如果没有找到用户信息，尝试获取一次
  if (!at_display_name.has_value()) {
    // 尝试获取群成员信息并保存
    co_await QQMessageFormatter::fetch_and_save_user_info(
        db_manager_, qq_bot, qq_user_id, event.group_id.value());

    // 更新显示名称
    at_display_name = db_manager_->query_user_display_name(
        "qq", qq_user_id, event.group_id.value_or(""));
  }

  // 判断是否显示发送者信息（基于配置）
  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    // Topic模式：获取对应topic的配置
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  // 设置最终显示文本
  if (show_sender && at_display_name.has_value()) {
    converted_segment.data["text"] =
        fmt::format("@{} ", at_display_name.value());
    PLUGIN_DEBUG("qq_to_tg", "转换QQ@消息: {} -> @{}", qq_user_id,
                 at_display_name.value());
  } else if (show_sender) {
    converted_segment.data["text"] = fmt::format("[@{}] ", qq_user_id);
  } else {
    // 不显示发送者，返回空文本
    converted_segment.data["text"] = "";
    PLUGIN_DEBUG("qq_to_tg", "QQ@消息不显示发送者: {}", qq_user_id);
  }

  co_return converted_segment;
}

} // namespace bridge::qq

#pragma once

#include "config.hpp"

#include <common/message_type.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace bridge::telegram {

/**
 * @brief Telegram消息格式化器
 *
 * 负责处理Telegram消息的格式化逻辑，包括发送者信息和回复处理
 */
class TelegramMessageFormatter {
public:
  /**
   * @brief 格式化消息发送者信息
   * @param event 消息事件
   * @param bridge_config 桥接配置
   * @param telegram_group_id Telegram群ID
   * @param message_to_send 消息段列表（输出参数）
   */
  static auto format_sender_info(
      const obcx::common::MessageEvent &event,
      const GroupBridgeConfig *bridge_config,
      const std::string &telegram_group_id,
      std::vector<obcx::common::MessageSegment> &message_to_send) -> void;

  /**
   * @brief 处理回复消息格式化
   * @param event 消息事件
   * @param reply_to_message_id 回复的QQ消息ID（如果找到）
   * @param message_to_send 消息段列表（输出参数）
   */
  static auto format_reply_message(
      const obcx::common::MessageEvent &event,
      const std::optional<std::string> &reply_to_message_id,
      std::vector<obcx::common::MessageSegment> &message_to_send) -> void;

private:
  /**
   * @brief 获取发送者显示名称
   * @param event 消息事件
   * @return 发送者显示名称
   */
  static auto get_sender_display_name(const obcx::common::MessageEvent &event)
      -> std::string;

  /**
   * @brief 检查是否是真实的回复消息（排除Topic结构回复）
   * @param event 消息事件
   * @return 是否是真实回复
   */
  static auto is_genuine_reply(const obcx::common::MessageEvent &event) -> bool;

  /**
   * @brief 检查消息段列表中是否已包含回复段
   * @param message_to_send 消息段列表
   * @return 是否已包含回复段
   */
  static auto has_reply_segment(
      const std::vector<obcx::common::MessageSegment> &message_to_send) -> bool;
};

} // namespace bridge::telegram

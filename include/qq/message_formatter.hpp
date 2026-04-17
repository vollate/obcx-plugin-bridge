#pragma once

#include "config.hpp"
#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>
#include <string>
#include <vector>

namespace bridge::qq {

/**
 * @brief QQ消息格式化器
 *
 * 负责处理QQ消息的格式化逻辑，包括发送者信息、回复处理和媒体组合
 */
class QQMessageFormatter {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit QQMessageFormatter(
      std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 格式化消息发送者信息
   * @param qq_bot QQ机器人实例
   * @param event 消息事件
   * @param bridge_config 桥接配置
   * @param qq_group_id QQ群ID
   * @param telegram_group_id Telegram群ID
   * @param topic_id Topic ID（-1表示群组模式）
   * @param message_to_send 消息段列表（输出参数）
   * @return 发送者显示名称
   */
  auto format_sender_info(obcx::core::IBot &qq_bot,
                          const obcx::common::MessageEvent &event,
                          const GroupBridgeConfig *bridge_config,
                          const std::string &qq_group_id,
                          const std::string &telegram_group_id,
                          int64_t topic_id,
                          obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<std::string>;

  /**
   * @brief 处理回复消息格式化
   * @param event 消息事件
   * @param message_to_send 消息段列表（输出参数）
   * @return 是否成功添加回复段
   */
  auto format_reply_message(const obcx::common::MessageEvent &event,
                            obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<bool>;

  /**
   * @brief 处理合并转发消息
   * @param qq_bot QQ机器人实例
   * @param telegram_bot Telegram机器人实例
   * @param segment 合并转发消息段
   * @param telegram_group_id Telegram群ID
   * @param topic_id Topic ID
   * @param message_to_send 消息段列表（输出参数）
   */
  auto process_forward_message(obcx::core::IBot &qq_bot,
                               obcx::core::IBot &telegram_bot,
                               const obcx::common::MessageSegment &segment,
                               const std::string &telegram_group_id,
                               int64_t topic_id,
                               obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理单个node消息段（自定义转发节点）
   * @param segment node消息段
   * @param message_to_send 消息段列表（输出参数）
   */
  auto process_node_message(const obcx::common::MessageSegment &segment,
                            obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理多图片MediaGroup发送
   * @param telegram_bot Telegram机器人实例
   * @param image_segments 图片消息段列表
   * @param other_segments 其他消息段列表
   * @param telegram_group_id Telegram群ID
   * @param topic_id Topic ID
   * @param sender_display_name 发送者显示名称
   * @param bridge_config 桥接配置
   * @param message_to_send 消息段列表
   * @param event 原始消息事件
   * @return 是否成功发送MediaGroup
   */
  auto send_media_group(
      obcx::core::IBot &telegram_bot,
      const std::vector<obcx::common::MessageSegment> &image_segments,
      const std::vector<obcx::common::MessageSegment> &other_segments,
      const std::string &telegram_group_id, int64_t topic_id,
      const std::string &sender_display_name,
      const GroupBridgeConfig *bridge_config,
      const obcx::common::Message &message_to_send,
      const obcx::common::MessageEvent &event) -> boost::asio::awaitable<bool>;

  static auto fetch_and_save_user_info(
      std::shared_ptr<storage::DatabaseManager> db_manager,
      obcx::core::IBot &qq_bot, const std::string &user_id,
      const std::string &group_id) -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 获取用户显示名称，如果数据库没有则异步获取
   * @param qq_bot QQ机器人实例
   * @param user_id 用户ID
   * @param group_id 群ID
   * @return 用户显示名称
   */
  auto get_user_display_name(obcx::core::IBot &qq_bot,
                             const std::string &user_id,
                             const std::string &group_id)
      -> boost::asio::awaitable<std::string>;

  /**
   * @brief 异步获取用户信息并保存到数据库
   * @param qq_bot QQ机器人实例
   * @param user_id 用户ID
   * @param group_id 群ID
   */
  auto fetch_user_info(obcx::core::IBot &qq_bot, const std::string &user_id,
                       const std::string &group_id)
      -> boost::asio::awaitable<void>;
};

} // namespace bridge::qq

#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge::qq {

/**
 * @brief QQ事件处理器
 *
 * 处理QQ的撤回、戳一戳等事件
 */
class QQEventHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit QQEventHandler(std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 处理QQ撤回事件
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event QQ撤回事件
   * @return 处理结果的awaitable
   */
  auto handle_recall_event(obcx::core::IBot &telegram_bot,
                           obcx::core::IBot &qq_bot, obcx::common::Event event)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理QQ戳一戳事件
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event QQ戳一戳通知事件
   * @return 处理结果的awaitable
   */
  auto handle_poke_event(obcx::core::IBot &telegram_bot,
                         obcx::core::IBot &qq_bot,
                         const obcx::common::NoticeEvent &event)
      -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 根据 type 和 id 获取戳一戳动作名称（基于 Mirai PokeMessage 定义）
   * @param poke_type 戳一戳类型
   * @param poke_id 戳一戳ID
   * @return 动作名称
   */
  static auto get_poke_action_name(int poke_type, int poke_id) -> std::string;

  /**
   * @brief 转义MarkdownV2特殊字符
   * @param text 原始文本
   * @return 转义后的文本
   */
  static auto escape_markdown_v2(const std::string &text) -> std::string;

  /**
   * @brief 获取用户显示名称，如果数据库没有则异步获取
   * @param qq_bot QQ机器人实例
   * @param user_id 用户ID
   * @param group_id 群ID
   * @return 用户显示名称
   */
  auto fetch_user_display_name(obcx::core::IBot &qq_bot,
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

#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>
#include <string_view>

namespace bridge::telegram {

/**
 * @brief Telegram命令处理器
 *
 * 处理各种Telegram命令，如/recall等
 */
class TelegramCommandHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit TelegramCommandHandler(
      std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 处理 /recall 命令
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 包含 /recall 命令的消息事件
   * @param qq_group_id 对应的QQ群ID
   * @return 处理结果的awaitable
   */
  auto handle_recall_command(obcx::core::IBot &telegram_bot,
                             obcx::core::IBot &qq_bot,
                             obcx::common::MessageEvent event,
                             std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理 /checkalive 命令
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 包含 /checkalive 命令的消息事件
   * @param qq_group_id 对应的QQ群ID
   * @return 处理结果的awaitable
   */
  auto handle_checkalive_command(obcx::core::IBot &telegram_bot,
                                 obcx::core::IBot &qq_bot,
                                 obcx::common::MessageEvent event,
                                 std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理 /poke 命令
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 包含 /poke 命令的消息事件
   * @param qq_group_id 对应的QQ群ID
   * @return 处理结果的awaitable
   */
  auto handle_poke_command(obcx::core::IBot &telegram_bot,
                           obcx::core::IBot &qq_bot,
                           obcx::common::MessageEvent event,
                           std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 发送回复消息
   * @param telegram_bot Telegram机器人实例
   * @param telegram_group_id Telegram群ID
   * @param reply_to_message_id 回复的消息ID
   * @param text 消息内容
   */
  auto send_reply_message(obcx::core::IBot &telegram_bot,
                          const std::string &telegram_group_id,
                          const std::string &reply_to_message_id,
                          const std::string &text)
      -> boost::asio::awaitable<void>;
};

} // namespace bridge::telegram

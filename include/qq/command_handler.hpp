#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge::qq {

/**
 * @brief QQ命令处理器
 *
 * 处理各种QQ命令，如/checkalive等
 */
class QQCommandHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit QQCommandHandler(
      std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 处理 /checkalive 命令
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 包含 /checkalive 命令的消息事件
   * @param telegram_group_id 对应的Telegram群ID
   * @return 处理结果的awaitable
   */
  auto handle_checkalive_command(obcx::core::IBot &telegram_bot,
                                 obcx::core::IBot &qq_bot,
                                 obcx::common::MessageEvent event,
                                 const std::string &telegram_group_id)
      -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 发送回复消息
   * @param qq_bot QQ机器人实例
   * @param qq_group_id QQ群ID
   * @param reply_to_message_id 回复的消息ID
   * @param text 消息内容
   */
  auto send_reply_message(obcx::core::IBot &qq_bot,
                          const std::string &qq_group_id,
                          const std::string &reply_to_message_id,
                          const std::string &text)
      -> boost::asio::awaitable<void>;
};

} // namespace bridge::qq

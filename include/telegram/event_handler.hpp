#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <functional>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge::telegram {

/**
 * @brief Telegram事件处理器
 *
 * 处理Telegram的消息删除、编辑等事件
 */
class TelegramEventHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   * @param forward_function 转发消息的函数
   */
  explicit TelegramEventHandler(
      std::shared_ptr<storage::DatabaseManager> db_manager,
      std::function<boost::asio::awaitable<void>(
          obcx::core::IBot &, obcx::core::IBot &, obcx::common::MessageEvent)>
          forward_function);

  /**
   * @brief 处理Telegram消息删除事件
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 删除事件
   * @return 处理结果的awaitable
   */
  auto handle_message_deleted(obcx::core::IBot &telegram_bot,
                              obcx::core::IBot &qq_bot,
                              obcx::common::Event event)
      -> boost::asio::awaitable<void>;

  /**
   * @brief 处理Telegram消息编辑事件
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event 编辑事件
   * @return 处理结果的awaitable
   */
  auto handle_message_edited(obcx::core::IBot &telegram_bot,
                             obcx::core::IBot &qq_bot,
                             obcx::common::MessageEvent event)
      -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;
  std::function<boost::asio::awaitable<void>(
      obcx::core::IBot &, obcx::core::IBot &, obcx::common::MessageEvent)>
      forward_function_;
};

} // namespace bridge::telegram

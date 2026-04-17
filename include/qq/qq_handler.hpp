#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge {

// Forward declarations
class RetryQueueManager;

namespace qq {
class QQMediaProcessor;
class QQCommandHandler;
class QQEventHandler;
class QQMessageFormatter;
} // namespace qq

/**
 * @brief QQ消息处理器
 *
 * 处理从QQ到Telegram的消息转发
 */
class QQHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器的引用
   * @param retry_manager 重试队列管理器（可选）
   */
  explicit QQHandler(
      std::shared_ptr<storage::DatabaseManager> db_manager,
      std::shared_ptr<RetryQueueManager> retry_manager = nullptr);

  /**
   * @brief 析构函数
   *
   * 需要显式声明以确保在实现文件中定义，
   * 避免不完整类型的unique_ptr析构问题
   */
  ~QQHandler();

  /**
   * @brief 将QQ消息转发到Telegram
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event QQ消息事件
   * @return 处理结果的awaitable
   */
  auto forward_to_telegram(obcx::core::IBot &telegram_bot,
                           obcx::core::IBot &qq_bot,
                           obcx::common::MessageEvent event)
      -> boost::asio::awaitable<void>;

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
  std::shared_ptr<RetryQueueManager> retry_manager_;

  // 子模块处理器
  std::unique_ptr<qq::QQMediaProcessor> media_processor_;
  std::unique_ptr<qq::QQCommandHandler> command_handler_;
  std::unique_ptr<qq::QQEventHandler> event_handler_;
  std::unique_ptr<qq::QQMessageFormatter> message_formatter_;
};

} // namespace bridge

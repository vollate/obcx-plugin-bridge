#pragma once

#include "common/message_type.hpp"
#include "database/manager.hpp"
#include "telegram/command_handler.hpp"
#include "telegram/event_handler.hpp"
#include "telegram/media_processor.hpp"

#include <boost/asio.hpp>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge {

// Forward declarations
class RetryQueueManager;

/**
 * @brief Telegram消息处理器
 *
 * 处理从Telegram到QQ的消息转发，使用模块化设计
 */
class TelegramHandler {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器的引用
   * @param retry_manager 重试队列管理器（可选）
   */
  explicit TelegramHandler(
      std::shared_ptr<storage::DatabaseManager> db_manager,
      std::shared_ptr<RetryQueueManager> retry_manager = nullptr);

  /**
   * @brief 将Telegram消息转发到QQ
   * @param telegram_bot Telegram机器人实例
   * @param qq_bot QQ机器人实例
   * @param event Telegram消息事件
   * @return 处理结果的awaitable
   */
  auto forward_to_qq(obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
                     obcx::common::MessageEvent event)
      -> boost::asio::awaitable<void>;

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

private:
  auto download_sticker_with_cache(obcx::core::IBot &telegram_bot,
                                   const obcx::core::MediaFileInfo &media_info,
                                   const std::string &bridge_files_dir)
      -> boost::asio::awaitable<std::optional<std::string>>;

  std::shared_ptr<storage::DatabaseManager> db_manager_;
  std::shared_ptr<RetryQueueManager> retry_manager_;
  std::unique_ptr<telegram::TelegramMediaProcessor> media_processor_;
  std::unique_ptr<telegram::TelegramCommandHandler> command_handler_;
  std::unique_ptr<telegram::TelegramEventHandler> event_handler_;
};

} // namespace bridge

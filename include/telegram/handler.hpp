#pragma once

#include "common/message_type.hpp"
#include "telegram/command_handler.hpp"
#include "telegram/event_handler.hpp"
#include "telegram/media_processor.hpp"

#include <boost/asio.hpp>
#include <interfaces/bot.hpp>
#include <memory>

namespace bridge {

// Forward declarations
class BridgeStateRepository;
class ReceivedMessageRepository;
class RetryQueueManager;

namespace telegram {
class TGMediaGroupBuffer;
} // namespace telegram

/**
 * @brief Telegram消息处理器
 *
 * 处理从Telegram到QQ的消息转发，使用模块化设计
 */
class TelegramHandler : public std::enable_shared_from_this<TelegramHandler> {
public:
  /**
   * @brief 构造函数
   * @param retry_manager 重试队列管理器（可选）
   * @param buffer_executor 用于驱动 media-group 缓冲区去抖定时器的 executor
   */
  explicit TelegramHandler(
      std::shared_ptr<RetryQueueManager> retry_manager,
      boost::asio::any_io_executor buffer_executor,
      std::shared_ptr<BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<ReceivedMessageRepository> received_message_repository =
          nullptr);

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

  /**
   * @brief Synchronously flush any buffered Telegram media-groups.
   *
   * Should be called by the plugin during shutdown so albums whose debounce
   * timer is still pending are not silently dropped.
   */
  void flush_pending_media_groups();

private:
  auto download_sticker_with_cache(obcx::core::IBot &telegram_bot,
                                   const obcx::core::MediaFileInfo &media_info,
                                   const std::string &bridge_files_dir)
      -> boost::asio::awaitable<std::optional<std::string>>;

  /// Forward a single Telegram album as one combined QQ message. Used by the
  /// media-group buffer's flush callback.
  auto forward_media_group_to_qq(obcx::core::IBot &telegram_bot,
                                 obcx::core::IBot &qq_bot,
                                 std::vector<obcx::common::MessageEvent> events)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<RetryQueueManager> retry_manager_;
  std::shared_ptr<BridgeStateRepository> state_repository_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
  std::unique_ptr<telegram::TelegramMediaProcessor> media_processor_;
  std::unique_ptr<telegram::TelegramCommandHandler> command_handler_;
  std::unique_ptr<telegram::TelegramEventHandler> event_handler_;
  std::shared_ptr<telegram::TGMediaGroupBuffer> media_group_buffer_;
  boost::asio::any_io_executor buffer_executor_;
};

} // namespace bridge

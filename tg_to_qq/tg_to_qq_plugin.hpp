#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <core/qq_bot.hpp>
#include <interfaces/plugin.hpp>
#include <memory>
#include <thread>

// Forward declarations
namespace bridge {
class TelegramHandler;
}
namespace storage {
class DatabaseManager;
}
namespace bridge {
class RetryQueueManager;
}

namespace plugins {

/**
 * @brief Telegram到QQ转发插件
 *
 * 只处理Telegram消息的转发到QQ
 * 使用BridgeBot的TelegramHandler进行消息处理
 */
class TGToQQPlugin : public obcx::interface::IPlugin {
public:
  TGToQQPlugin();
  ~TGToQQPlugin() override;

  // IPlugin interface
  [[nodiscard]] auto get_name() const -> std::string override;
  [[nodiscard]] auto get_version() const -> std::string override;
  [[nodiscard]] auto get_description() const -> std::string override;
  auto initialize() -> bool override;
  void deinitialize() override;
  void shutdown() override;

private:
  obcx::core::QQBot *qq_bot_{nullptr};
  // 简化配置
  struct Config {
    std::string database_file = "bridge_bot.db";
    bool enable_retry_queue = false;
  };

  auto load_configuration() -> bool;
  auto handle_tg_message(obcx::core::IBot &bot,
                         const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<void>;

  // Configuration
  Config config_;

  // Bridge components
  std::shared_ptr<storage::DatabaseManager> db_manager_;
  std::shared_ptr<bridge::RetryQueueManager> retry_manager_;
  std::unique_ptr<bridge::TelegramHandler> telegram_handler_;

  // Retry queue io_context (non-static to avoid reload issues)
  std::unique_ptr<boost::asio::io_context> retry_io_context_;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      retry_work_guard_;
  std::unique_ptr<std::thread> retry_io_thread_;
};

} // namespace plugins

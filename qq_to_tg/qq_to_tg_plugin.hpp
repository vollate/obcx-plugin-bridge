#pragma once

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <core/tg_bot.hpp>
#include <interfaces/plugin.hpp>
#include <memory>
#include <mutex>
#include <thread>

// Forward declarations
namespace bridge {
class QQHandler;
}

namespace storage {
class DatabaseManager;
}

namespace bridge {
class RetryQueueManager;
}

namespace plugins {
/**
 * @brief QQ到Telegram转发插件
 *
 * 只处理QQ消息的转发到Telegram
 * 使用BridgeBot的QQHandler进行消息处理
 */
class QQToTGPlugin : public obcx::interface::IPlugin {
public:
  QQToTGPlugin();

  ~QQToTGPlugin() override;

  // IPlugin interface
  [[nodiscard]] auto get_name() const -> std::string override;

  [[nodiscard]] auto get_version() const -> std::string override;

  [[nodiscard]] auto get_description() const -> std::string override;

  auto initialize() -> bool override;

  void deinitialize() override;

  void shutdown() override;

private:
  struct RuntimeState;

  struct Config {
    std::string database_file = "bridge_bot.db";
    bool enable_retry_queue = false;
  };

  auto load_configuration() -> bool;

  static auto handle_qq_message(std::shared_ptr<RuntimeState> state,
                                obcx::core::IBot &bot,
                                const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<void>;

  static auto handle_qq_heartbeat(std::shared_ptr<RuntimeState> state,
                                  obcx::core::IBot &bot,
                                  const obcx::common::HeartbeatEvent &event)
      -> boost::asio::awaitable<void>;

  static auto handle_qq_notice(std::shared_ptr<RuntimeState> state,
                               obcx::core::IBot &bot,
                               const obcx::common::NoticeEvent &event)
      -> boost::asio::awaitable<void>;

  // Configuration
  Config config_;

  // Bridge components
  std::shared_ptr<storage::DatabaseManager> db_manager_;
  std::shared_ptr<bridge::RetryQueueManager> retry_manager_;
  std::shared_ptr<RuntimeState> runtime_state_;

  // Retry queue io_context (non-static to avoid reload issues)
  std::unique_ptr<boost::asio::io_context> retry_io_context_;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      retry_work_guard_;
  std::unique_ptr<std::thread> retry_io_thread_;
};
} // namespace plugins

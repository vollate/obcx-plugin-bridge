#pragma once

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <core/qq_bot.hpp>
#include <interfaces/plugin.hpp>
#include <memory>
#include <mutex>
#include <thread>

namespace bridge {
class TelegramHandler;
}
namespace bridge {
class BridgeStateRepository;
class ReceivedMessageRepository;
class RetryQueueManager;
} // namespace bridge
namespace obcx::core {
class DbManager;
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
  static auto handle_tg_message(std::shared_ptr<RuntimeState> state,
                                obcx::core::IBot &bot,
                                const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<void>;

  Config config_;

  std::shared_ptr<obcx::core::DbManager> bridge_db_manager_;
  std::shared_ptr<bridge::BridgeStateRepository> bridge_state_repository_;
  std::shared_ptr<bridge::ReceivedMessageRepository>
      received_message_repository_;
  std::shared_ptr<bridge::RetryQueueManager> retry_manager_;
  std::shared_ptr<RuntimeState> runtime_state_;

  // Dedicated io_context for the retry queue, kept independent of the
  // plugin host's executor so reload cycles don't tear it down mid-flight.
  std::unique_ptr<boost::asio::io_context> retry_io_context_;
  std::unique_ptr<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      retry_work_guard_;
  std::unique_ptr<std::thread> retry_io_thread_;
};

} // namespace plugins

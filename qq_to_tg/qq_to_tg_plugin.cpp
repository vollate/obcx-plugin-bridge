#include "qq_to_tg_plugin.hpp"
#include "config.hpp"
#include "database/manager.hpp"
#include "qq/handler.hpp"
#include "retry_queue_manager.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <core/tg_bot.hpp>
#include <nlohmann/json.hpp>

namespace plugins {
QQToTGPlugin::QQToTGPlugin() {
  PLUGIN_DEBUG(get_name(), "QQToTGPlugin constructor called");
}

QQToTGPlugin::~QQToTGPlugin() {
  shutdown();
  PLUGIN_DEBUG(get_name(), "QQToTGPlugin destructor called");
}

auto QQToTGPlugin::get_name() const -> std::string { return "qq_to_tg"; }

auto QQToTGPlugin::get_version() const -> std::string { return "1.0.0"; }

auto QQToTGPlugin::get_description() const -> std::string {
  return "QQ to Telegram message forwarding plugin (simplified version)";
}

auto QQToTGPlugin::initialize() -> bool {
  try {
    PLUGIN_INFO(get_name(), "Initializing QQ to TG Plugin...");

    // Initialize bridge configuration system
    bridge::initialize_config();

    // Load configuration
    if (!load_configuration()) {
      PLUGIN_ERROR(get_name(), "Failed to load plugin configuration");
      return false;
    }

    // Initialize database manager (singleton)
    db_manager_ = storage::DatabaseManager::instance(config_.database_file);
    if (!db_manager_ || !db_manager_->initialize()) {
      PLUGIN_ERROR(get_name(), "Failed to initialize database");
      return false;
    }

    // Initialize retry queue manager if enabled (in-memory, non-persistent)
    if (config_.enable_retry_queue) {
      // Create a dedicated io_context for retry queue (non-static)
      retry_io_context_ = std::make_unique<boost::asio::io_context>();

      // Create work guard to keep io_context running
      retry_work_guard_ = std::make_unique<boost::asio::executor_work_guard<
          boost::asio::io_context::executor_type>>(
          retry_io_context_->get_executor());

      // Create retry manager
      retry_manager_ =
          std::make_shared<bridge::RetryQueueManager>(*retry_io_context_);

      // Start io_context in a dedicated thread
      retry_io_thread_ = std::make_unique<std::thread>([this]() {
        PLUGIN_INFO(get_name(), "Retry queue io_context thread started");
        retry_io_context_->run();
        PLUGIN_INFO(get_name(), "Retry queue io_context thread stopped");
      });

      // Register callback for sending messages to Telegram
      retry_manager_->register_message_send_callback(
          "telegram",
          [this](const bridge::MessageRetryEntry &retry_info,
                 const obcx::common::Message &message)
              -> boost::asio::awaitable<std::optional<std::string>> {
            // Find Telegram bot
            obcx::core::TGBot *tg_bot = nullptr;
            {
              auto [lock, bots] = get_bots();
              for (auto &bot_ptr : bots) {
                if (auto *tg =
                        dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
                  tg_bot = tg;
                  break;
                }
              }
            }

            if (!tg_bot) {
              PLUGIN_WARN(get_name(),
                          "Telegram bot not found for retry callback");
              co_return std::nullopt;
            }

            try {
              std::string response;
              if (retry_info.target_topic_id > 0) {
                response = co_await tg_bot->send_topic_message(
                    retry_info.group_id, retry_info.target_topic_id, message);
              } else {
                response = co_await tg_bot->send_group_message(
                    retry_info.group_id, message);
              }

              // Parse response to get message_id
              auto json_response = nlohmann::json::parse(response);
              if (json_response.contains("result") &&
                  json_response["result"].contains("message_id")) {
                auto msg_id =
                    json_response["result"]["message_id"].get<int64_t>();
                co_return std::to_string(msg_id);
              }
              co_return std::nullopt;
            } catch (const std::exception &e) {
              PLUGIN_ERROR(get_name(), "Retry send to Telegram failed: {}",
                           e.what());
              co_return std::nullopt;
            }
          });
      PLUGIN_INFO(get_name(), "Registered Telegram message retry callback");

      // Start the retry manager processing loop
      retry_manager_->start();
      PLUGIN_INFO(get_name(), "Retry queue manager started");
    }

    // Create QQHandler instance
    qq_handler_ =
        std::make_unique<bridge::QQHandler>(db_manager_, retry_manager_);

    // Register event callbacks
    try {
      // 获取所有bot实例的带锁访问
      auto [lock, bots] = get_bots();

      // 找到QQ bot并注册消息回调和心跳回调
      for (auto &bot_ptr : bots) {
        if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(bot_ptr.get())) {
          // 注册消息事件回调
          qq_bot->on_event<obcx::common::MessageEvent>(
              [this](obcx::core::IBot &bot,
                     const obcx::common::MessageEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_message(bot, event);
              });
          PLUGIN_INFO(get_name(),
                      "Registered QQ message callback for QQ to TG plugin");

          // 注册心跳事件回调
          qq_bot->on_event<obcx::common::HeartbeatEvent>(
              [this](obcx::core::IBot &bot,
                     const obcx::common::HeartbeatEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_heartbeat(bot, event);
              });
          PLUGIN_INFO(get_name(),
                      "Registered QQ heartbeat callback for QQ to TG plugin");

          // 注册通知事件回调（用于处理撤回消息等）
          qq_bot->on_event<obcx::common::NoticeEvent>(
              [this](obcx::core::IBot &bot,
                     const obcx::common::NoticeEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_notice(bot, event);
              });
          PLUGIN_INFO(get_name(),
                      "Registered QQ notice callback for QQ to TG plugin");

          break;
        }
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR(get_name(), "Failed to register callbacks: {}", e.what());
      return false;
    }

    PLUGIN_INFO(get_name(), "QQ to TG Plugin initialized successfully");
    return true;
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(),
                 "Exception during QQ to TG Plugin initialization: {}",
                 e.what());
    return false;
  }
}

void QQToTGPlugin::deinitialize() {
  try {
    PLUGIN_INFO(get_name(), "Deinitializing QQ to TG Plugin...");
    // Note: Bot callbacks will be automatically cleaned up when plugin is
    // unloaded If needed, specific cleanup can be added here
    PLUGIN_INFO(get_name(), "QQ to TG Plugin deinitialized successfully");
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(),
                 "Exception during QQ to TG Plugin deinitialization: {}",
                 e.what());
  }
}

void QQToTGPlugin::shutdown() {
  try {
    PLUGIN_INFO(get_name(), "Shutting down QQ to TG Plugin...");

    // Clear cached bot pointer first
    tg_bot_ = nullptr;

    // Stop retry manager if running (this cancels any pending async operations)
    if (retry_manager_) {
      retry_manager_->stop();
      retry_manager_.reset();
    }

    // Release work guard to allow io_context to stop
    retry_work_guard_.reset();

    // Stop io_context and join the thread
    if (retry_io_context_) {
      retry_io_context_->stop();
    }
    if (retry_io_thread_ && retry_io_thread_->joinable()) {
      retry_io_thread_->join();
    }
    retry_io_thread_.reset();
    retry_io_context_.reset();

    // Release QQ handler
    qq_handler_.reset();

    // Don't reset db_manager_ - it's a singleton shared with other plugins
    db_manager_ = nullptr;

    PLUGIN_INFO(get_name(), "QQ to TG Plugin shutdown complete");
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(), "Exception during QQ to TG Plugin shutdown: {}",
                 e.what());
  }
}

auto QQToTGPlugin::handle_qq_message(obcx::core::IBot &bot,
                                     const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<void> {
  // 确保这是QQ bot的消息
  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    PLUGIN_INFO(get_name(),
                "QQ to TG Plugin: Processing QQ message from group {}",
                event.group_id.value_or("unknown"));

    try {
      if (!tg_bot_) {
        auto [lock, bots] = get_bots();

        for (auto &bot_ptr : bots) {
          if (auto *tg = dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
            tg_bot_ = tg;
            break;
          }
        }
      }

      if (tg_bot_ && qq_handler_) {
        PLUGIN_INFO(get_name(),
                    "Found Telegram bot, performing QQ->TG message forwarding "
                    "using QQHandler");
        co_await qq_handler_->forward_to_telegram(*tg_bot_, *qq_bot, event);
      } else {
        PLUGIN_WARN(
            get_name(),
            "Telegram bot or QQHandler not found for QQ->TG forwarding");
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR(get_name(), "Error accessing bot list: {}", e.what());
    }
  }

  co_return;
}

auto QQToTGPlugin::handle_qq_heartbeat(
    obcx::core::IBot &bot, const obcx::common::HeartbeatEvent &event)
    -> boost::asio::awaitable<void> {
  // 确保这是QQ bot的心跳
  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    // 更新QQ平台的心跳时间
    if (db_manager_) {
      db_manager_->update_platform_heartbeat("qq",
                                             std::chrono::system_clock::now());
      PLUGIN_DEBUG(get_name(), "QQ platform heartbeat updated, interval: {}ms",
                   event.interval);
    }
  }

  co_return;
}

auto QQToTGPlugin::handle_qq_notice(obcx::core::IBot &bot,
                                    const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  // 确保这是QQ bot的通知
  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    PLUGIN_DEBUG(get_name(), "QQ to TG Plugin: Processing QQ notice, type: {}",
                 event.notice_type);

    try {
      if (!tg_bot_) {
        auto [lock, bots] = get_bots();

        for (auto &bot_ptr : bots) {
          if (auto *tg = dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
            tg_bot_ = tg;
            break;
          }
        }
      }

      if (tg_bot_ && qq_handler_) {
        // 将 NoticeEvent 转换为 Event variant 并传递给 handler
        obcx::common::Event event_variant = event;
        co_await qq_handler_->handle_recall_event(*tg_bot_, *qq_bot,
                                                  event_variant);
      } else {
        PLUGIN_WARN(get_name(),
                    "Telegram bot or QQHandler not found for notice handling");
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR(get_name(), "Error handling QQ notice: {}", e.what());
    }
  }

  co_return;
}

auto QQToTGPlugin::load_configuration() -> bool {
  try {
    // 从插件配置加载设置
    config_.database_file = get_config_value<std::string>("database_file")
                                .value_or("bridge_bot.db");
    config_.enable_retry_queue =
        get_config_value<bool>("enable_retry_queue").value_or(false);

    PLUGIN_INFO(get_name(),
                "QQ to TG configuration loaded: database={}, retry_queue={}",
                config_.database_file, config_.enable_retry_queue);
    return true;
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(), "Failed to load QQ to TG configuration: {}",
                 e.what());
    return false;
  }
}

} // namespace plugins

// Export the plugin
OBCX_PLUGIN_EXPORT(plugins::QQToTGPlugin)

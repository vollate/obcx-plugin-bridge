#include "tg_to_qq_plugin.hpp"
#include "config.hpp"
#include "database/manager.hpp"
#include "retry_queue_manager.hpp"
#include "telegram/handler.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <core/tg_bot.hpp>
#include <nlohmann/json.hpp>

namespace plugins {
TGToQQPlugin::TGToQQPlugin() {
  PLUGIN_DEBUG("tg_to_qq", "TGToQQPlugin constructor called");
}

TGToQQPlugin::~TGToQQPlugin() {
  shutdown();
  PLUGIN_DEBUG("tg_to_qq", "TGToQQPlugin destructor called");
}

auto TGToQQPlugin::get_name() const -> std::string { return "tg_to_qq"; }

auto TGToQQPlugin::get_version() const -> std::string { return "1.0.0"; }

auto TGToQQPlugin::get_description() const -> std::string {
  return "Telegram to QQ message forwarding plugin (simplified version)";
}

bool TGToQQPlugin::initialize() {
  try {
    PLUGIN_INFO(get_name(), "Initializing TG to QQ Plugin...");

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

      // Register callback for sending messages to QQ
      retry_manager_->register_message_send_callback(
          "qq",
          [this](const bridge::MessageRetryEntry &retry_info,
                 const obcx::common::Message &message)
              -> boost::asio::awaitable<std::optional<std::string>> {
            // Find QQ bot
            obcx::core::QQBot *qq_bot = nullptr;
            {
              auto [lock, bots] = get_bots();
              for (auto &bot_ptr : bots) {
                if (auto *qq =
                        dynamic_cast<obcx::core::QQBot *>(bot_ptr.get())) {
                  qq_bot = qq;
                  break;
                }
              }
            }

            if (!qq_bot) {
              PLUGIN_WARN(get_name(), "QQ bot not found for retry callback");
              co_return std::nullopt;
            }

            try {
              std::string response = co_await qq_bot->send_group_message(
                  retry_info.group_id, message);

              // Parse response to get message_id
              auto json_response = nlohmann::json::parse(response);
              if (json_response.contains("data") &&
                  json_response["data"].contains("message_id")) {
                auto msg_id =
                    json_response["data"]["message_id"].get<int64_t>();
                co_return std::to_string(msg_id);
              }
              co_return std::nullopt;
            } catch (const std::exception &e) {
              PLUGIN_ERROR(get_name(), "Retry send to QQ failed: {}", e.what());
              co_return std::nullopt;
            }
          });
      PLUGIN_INFO(get_name(), "Registered QQ message retry callback");

      // Start the retry manager processing loop
      retry_manager_->start();
      PLUGIN_INFO(get_name(), "Retry queue manager started");
    }

    // Create TelegramHandler instance
    telegram_handler_ =
        std::make_unique<bridge::TelegramHandler>(db_manager_, retry_manager_);

    // Register event callbacks
    try {
      // 获取所有bot实例的带锁访问
      auto [lock, bots] = get_bots();

      // 找到Telegram bot并注册消息回调
      for (auto &bot_ptr : bots) {
        if (auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
          tg_bot->on_event<obcx::common::MessageEvent>(
              [this](obcx::core::IBot &bot,
                     const obcx::common::MessageEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_tg_message(bot, event);
              });
          PLUGIN_INFO(
              get_name(),
              "Registered Telegram message callback for TG to QQ plugin");
          break;
        }
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR(get_name(), "Failed to register callbacks: {}", e.what());
      return false;
    }

    PLUGIN_INFO(get_name(), "TG to QQ Plugin initialized successfully");
    return true;
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(),
                 "Exception during TG to QQ Plugin initialization: {}",
                 e.what());
    return false;
  }
}

void TGToQQPlugin::deinitialize() {
  try {
    PLUGIN_INFO(get_name(), "Deinitializing TG to QQ Plugin...");
    // Note: Bot callbacks will be automatically cleaned up when plugin is
    // unloaded If needed, specific cleanup can be added here
    PLUGIN_INFO(get_name(), "TG to QQ Plugin deinitialized successfully");
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(),
                 "Exception during TG to QQ Plugin deinitialization: {}",
                 e.what());
  }
}

void TGToQQPlugin::shutdown() {
  try {
    PLUGIN_INFO(get_name(), "Shutting down TG to QQ Plugin...");

    // Clear cached bot pointer first
    qq_bot_ = nullptr;

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

    // Release Telegram handler
    telegram_handler_.reset();

    // Don't reset db_manager_ - it's a singleton shared with other plugins
    db_manager_ = nullptr;

    PLUGIN_INFO(get_name(), "TG to QQ Plugin shutdown complete");
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(), "Exception during TG to QQ Plugin shutdown: {}",
                 e.what());
  }
}

boost::asio::awaitable<void> TGToQQPlugin::handle_tg_message(
    obcx::core::IBot &bot, const obcx::common::MessageEvent &event) {
  // 确保这是Telegram bot的消息
  if (auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&bot)) {
    PLUGIN_INFO(get_name(),
                "TG to QQ Plugin: Processing Telegram message from chat {}",
                event.group_id.value_or("unknown"));

    try {
      // 获取所有bot实例的带锁访问
      if (qq_bot_ == nullptr) {
        auto [lock, bots] = get_bots();

        // 找到QQ bot
        for (auto &bot_ptr : bots) {
          if (auto *qq = dynamic_cast<obcx::core::QQBot *>(bot_ptr.get())) {
            qq_bot_ = qq;
            break;
          }
        }
      }

      if (qq_bot_ != nullptr && telegram_handler_) {
        // Check if this is an edited message
        bool is_edited = (event.sub_type == "edited") ||
                         (event.data.contains("is_edited") &&
                          event.data["is_edited"].get<bool>());

        if (is_edited) {
          PLUGIN_INFO(get_name(),
                      "Detected edited message, handling as edit event");
          co_await telegram_handler_->handle_message_edited(*tg_bot, *qq_bot_,
                                                            event);
        } else {
          PLUGIN_INFO(
              get_name(),
              "Found QQ bot, performing TG->QQ message forwarding using "
              "TelegramHandler");
          co_await telegram_handler_->forward_to_qq(*tg_bot, *qq_bot_, event);
        }
      } else {
        PLUGIN_WARN(
            get_name(),
            "QQ bot or TelegramHandler not found for TG->QQ forwarding");
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR(get_name(), "Error accessing bot list: {}", e.what());
    }
  }

  co_return;
}

auto TGToQQPlugin::load_configuration() -> bool {
  try {
    config_.database_file = get_config_value<std::string>("database_file")
                                .value_or("bridge_bot.db");
    config_.enable_retry_queue =
        get_config_value<bool>("enable_retry_queue").value_or(false);

    PLUGIN_INFO(get_name(),
                "TG to QQ configuration loaded: database={}, retry_queue={}",
                config_.database_file, config_.enable_retry_queue);
    return true;
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(), "Failed to load TG to QQ configuration: {}",
                 e.what());
    return false;
  }
}

} // namespace plugins

// Export the plugin
OBCX_PLUGIN_EXPORT(plugins::TGToQQPlugin)

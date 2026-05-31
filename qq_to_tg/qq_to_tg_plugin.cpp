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

struct QQToTGPlugin::RuntimeState {
  std::atomic_bool shutting_down{false};
  std::mutex mutex;
  obcx::core::TGBot *tg_bot{nullptr};
  std::shared_ptr<bridge::QQHandler> qq_handler;
  std::shared_ptr<storage::DatabaseManager> db_manager;
};

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

    bridge::initialize_config();

    if (!load_configuration()) {
      PLUGIN_ERROR(get_name(), "Failed to load plugin configuration");
      return false;
    }

    db_manager_ = storage::DatabaseManager::instance(config_.database_file);
    if (!db_manager_ || !db_manager_->initialize()) {
      PLUGIN_ERROR(get_name(), "Failed to initialize database");
      return false;
    }

    runtime_state_ = std::make_shared<RuntimeState>();
    runtime_state_->db_manager = db_manager_;

    // Retry queue is in-memory (non-persistent); each plugin instance gets its
    // own io_context to avoid collisions on hot-reload.
    if (config_.enable_retry_queue) {
      retry_io_context_ = std::make_unique<boost::asio::io_context>();

      retry_work_guard_ = std::make_unique<boost::asio::executor_work_guard<
          boost::asio::io_context::executor_type>>(
          retry_io_context_->get_executor());

      retry_manager_ =
          std::make_shared<bridge::RetryQueueManager>(*retry_io_context_);

      retry_io_thread_ = std::make_unique<std::thread>([this]() -> void {
        PLUGIN_INFO(get_name(), "Retry queue io_context thread started");
        retry_io_context_->run();
        PLUGIN_INFO(get_name(), "Retry queue io_context thread stopped");
      });

      auto runtime_state = runtime_state_;
      retry_manager_->register_message_send_callback(
          "telegram",
          [runtime_state](const bridge::MessageRetryEntry &retry_info,
                          const obcx::common::Message &message)
              -> boost::asio::awaitable<std::optional<std::string>> {
            if (runtime_state->shutting_down.load(std::memory_order_acquire)) {
              co_return std::nullopt;
            }

            obcx::core::TGBot *tg_bot = nullptr;
            {
              auto [lock, bots] = obcx::interface::IPlugin::get_bots();
              for (auto &bot_ptr : bots) {
                if (auto *tg =
                        dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
                  tg_bot = tg;
                  break;
                }
              }
            }

            if (!tg_bot) {
              PLUGIN_WARN("qq_to_tg",
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

              auto json_response = nlohmann::json::parse(response);
              if (json_response.contains("result") &&
                  json_response["result"].contains("message_id")) {
                auto msg_id =
                    json_response["result"]["message_id"].get<int64_t>();
                co_return std::to_string(msg_id);
              }
              co_return std::nullopt;
            } catch (const std::exception &e) {
              PLUGIN_ERROR("qq_to_tg", "Retry send to Telegram failed: {}",
                           e.what());
              co_return std::nullopt;
            }
          });
      PLUGIN_INFO(get_name(), "Registered Telegram message retry callback");

      retry_manager_->start();
      PLUGIN_INFO(get_name(), "Retry queue manager started");
    }

    runtime_state_->qq_handler =
        std::make_shared<bridge::QQHandler>(db_manager_, retry_manager_);

    try {
      auto [lock, bots] = get_bots();

      for (auto &bot_ptr : bots) {
        if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(bot_ptr.get())) {
          auto runtime_state = runtime_state_;
          qq_bot->on_event<obcx::common::MessageEvent>(
              [runtime_state](obcx::core::IBot &bot,
                              const obcx::common::MessageEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_message(runtime_state, bot, event);
              });
          PLUGIN_INFO(get_name(),
                      "Registered QQ message callback for QQ to TG plugin");

          qq_bot->on_event<obcx::common::HeartbeatEvent>(
              [runtime_state](obcx::core::IBot &bot,
                              const obcx::common::HeartbeatEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_heartbeat(runtime_state, bot, event);
              });
          PLUGIN_INFO(get_name(),
                      "Registered QQ heartbeat callback for QQ to TG plugin");

          qq_bot->on_event<obcx::common::NoticeEvent>(
              [runtime_state](obcx::core::IBot &bot,
                              const obcx::common::NoticeEvent &event)
                  -> boost::asio::awaitable<void> {
                co_await handle_qq_notice(runtime_state, bot, event);
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

    if (runtime_state_) {
      runtime_state_->shutting_down.store(true, std::memory_order_release);
      std::scoped_lock lock(runtime_state_->mutex);
      runtime_state_->tg_bot = nullptr;
      runtime_state_->qq_handler.reset();
      runtime_state_->db_manager.reset();
    }

    // Stop retry manager first so any in-flight retry coroutines are cancelled
    // before we drop the io_context.
    if (retry_manager_) {
      retry_manager_->stop();
      retry_manager_.reset();
    }

    retry_work_guard_.reset();

    if (retry_io_context_) {
      retry_io_context_->stop();
    }
    if (retry_io_thread_ && retry_io_thread_->joinable()) {
      retry_io_thread_->join();
    }
    retry_io_thread_.reset();
    retry_io_context_.reset();

    // db_manager_ is a singleton shared with other plugins — drop our handle
    // but don't destroy the underlying instance.
    db_manager_ = nullptr;

    PLUGIN_INFO(get_name(), "QQ to TG Plugin shutdown complete");
  } catch (const std::exception &e) {
    PLUGIN_ERROR(get_name(), "Exception during QQ to TG Plugin shutdown: {}",
                 e.what());
  }
}

auto QQToTGPlugin::handle_qq_message(std::shared_ptr<RuntimeState> state,
                                     obcx::core::IBot &bot,
                                     const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<void> {
  if (!state || state->shutting_down.load(std::memory_order_acquire)) {
    co_return;
  }

  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    PLUGIN_INFO("qq_to_tg",
                "QQ to TG Plugin: Processing QQ message from group {}",
                event.group_id.value_or("unknown"));

    try {
      obcx::core::TGBot *tg_bot = nullptr;
      std::shared_ptr<bridge::QQHandler> qq_handler;

      {
        std::scoped_lock state_lock(state->mutex);
        tg_bot = state->tg_bot;
        qq_handler = state->qq_handler;
      }

      if (!tg_bot) {
        auto [lock, bots] = obcx::interface::IPlugin::get_bots();

        for (auto &bot_ptr : bots) {
          if (auto *tg = dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
            tg_bot = tg;
            break;
          }
        }

        if (tg_bot != nullptr) {
          std::scoped_lock state_lock(state->mutex);
          if (!state->shutting_down.load(std::memory_order_acquire)) {
            state->tg_bot = tg_bot;
          }
        }
      }

      if (state->shutting_down.load(std::memory_order_acquire)) {
        co_return;
      }

      if (tg_bot && qq_handler) {
        PLUGIN_INFO("qq_to_tg",
                    "Found Telegram bot, performing QQ->TG message forwarding "
                    "using QQHandler");
        co_await qq_handler->forward_to_telegram(*tg_bot, *qq_bot, event);
      } else {
        PLUGIN_WARN(
            "qq_to_tg",
            "Telegram bot or QQHandler not found for QQ->TG forwarding");
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR("qq_to_tg", "Error accessing bot list: {}", e.what());
    }
  }

  co_return;
}

auto QQToTGPlugin::handle_qq_heartbeat(
    std::shared_ptr<RuntimeState> state, obcx::core::IBot &bot,
    const obcx::common::HeartbeatEvent &event) -> boost::asio::awaitable<void> {
  if (!state || state->shutting_down.load(std::memory_order_acquire)) {
    co_return;
  }

  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    std::shared_ptr<storage::DatabaseManager> db_manager;
    {
      std::scoped_lock state_lock(state->mutex);
      db_manager = state->db_manager;
    }

    if (db_manager) {
      db_manager->update_platform_heartbeat("qq",
                                            std::chrono::system_clock::now());
      PLUGIN_DEBUG("qq_to_tg", "QQ platform heartbeat updated, interval: {}ms",
                   event.interval);
    }
  }

  co_return;
}

auto QQToTGPlugin::handle_qq_notice(std::shared_ptr<RuntimeState> state,
                                    obcx::core::IBot &bot,
                                    const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  if (!state || state->shutting_down.load(std::memory_order_acquire)) {
    co_return;
  }

  if (auto *qq_bot = dynamic_cast<obcx::core::QQBot *>(&bot)) {
    PLUGIN_DEBUG("qq_to_tg", "QQ to TG Plugin: Processing QQ notice, type: {}",
                 event.notice_type);

    try {
      obcx::core::TGBot *tg_bot = nullptr;
      std::shared_ptr<bridge::QQHandler> qq_handler;

      {
        std::scoped_lock state_lock(state->mutex);
        tg_bot = state->tg_bot;
        qq_handler = state->qq_handler;
      }

      if (!tg_bot) {
        auto [lock, bots] = obcx::interface::IPlugin::get_bots();

        for (auto &bot_ptr : bots) {
          if (auto *tg = dynamic_cast<obcx::core::TGBot *>(bot_ptr.get())) {
            tg_bot = tg;
            break;
          }
        }

        if (tg_bot != nullptr) {
          std::scoped_lock state_lock(state->mutex);
          if (!state->shutting_down.load(std::memory_order_acquire)) {
            state->tg_bot = tg_bot;
          }
        }
      }

      if (state->shutting_down.load(std::memory_order_acquire)) {
        co_return;
      }

      if (tg_bot && qq_handler) {
        obcx::common::Event event_variant = event;
        co_await qq_handler->handle_recall_event(*tg_bot, *qq_bot,
                                                 event_variant);
      } else {
        PLUGIN_WARN("qq_to_tg",
                    "Telegram bot or QQHandler not found for notice handling");
      }
    } catch (const std::exception &e) {
      PLUGIN_ERROR("qq_to_tg", "Error handling QQ notice: {}", e.what());
    }
  }

  co_return;
}

auto QQToTGPlugin::load_configuration() -> bool {
  try {
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

OBCX_PLUGIN_EXPORT(plugins::QQToTGPlugin)

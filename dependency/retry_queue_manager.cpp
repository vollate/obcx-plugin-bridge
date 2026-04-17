#include "retry_queue_manager.hpp"

#include <cmath>
#include <common/logger.hpp>
#include <sstream>

namespace bridge {

RetryQueueManager::RetryQueueManager(boost::asio::io_context &io_context)
    : io_context_(io_context),
      retry_timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
      running_(false) {
  PLUGIN_INFO("bridge", "RetryQueueManager initialized (in-memory mode)");
}

RetryQueueManager::~RetryQueueManager() { stop(); }

void RetryQueueManager::start() {

  bool expect_running_ = false;
  if (!running_.compare_exchange_strong(expect_running_, true)) {
    PLUGIN_WARN("bridge", "RetryQueueManager already running");
    return;
  }

  PLUGIN_INFO("bridge", "Starting RetryQueueManager");

  // Start retry queue processing coroutine
  boost::asio::co_spawn(io_context_, process_retry_queues(),
                        boost::asio::detached);
}

void RetryQueueManager::stop() {
  if (!running_) {
    return;
  }

  PLUGIN_INFO("bridge", "Stopping RetryQueueManager");
  running_ = false;

  if (retry_timer_) {
    retry_timer_->cancel();
  }

  // Clear all pending retries
  {
    std::lock_guard<std::mutex> lock(message_retry_mutex_);
    size_t msg_count = message_retry_queue_.size();
    message_retry_queue_.clear();
    if (msg_count > 0) {
      PLUGIN_INFO("bridge", "Cleared {} pending message retries", msg_count);
    }
  }
  {
    std::lock_guard<std::mutex> lock(media_retry_mutex_);
    size_t media_count = media_retry_queue_.size();
    media_retry_queue_.clear();
    if (media_count > 0) {
      PLUGIN_INFO("bridge", "Cleared {} pending media download retries",
                  media_count);
    }
  }
}

void RetryQueueManager::add_message_retry(
    const std::string &source_platform, const std::string &target_platform,
    const std::string &source_message_id, const obcx::common::Message &message,
    const std::string &group_id, const std::string &source_group_id,
    int64_t target_topic_id, int max_retries,
    const std::string &failure_reason) {

  MessageRetryEntry entry;
  entry.source_platform = source_platform;
  entry.target_platform = target_platform;
  entry.source_message_id = source_message_id;
  entry.message = message; // Store directly, no serialization
  entry.group_id = group_id;
  entry.source_group_id = source_group_id;
  entry.target_topic_id = target_topic_id;
  entry.retry_count = 0;
  entry.max_retry_count = max_retries;
  entry.failure_reason = failure_reason;
  entry.created_at = std::chrono::system_clock::now();
  entry.next_retry_at =
      calculate_next_retry_time(0, DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS);

  {
    std::lock_guard<std::mutex> lock(message_retry_mutex_);
    message_retry_queue_.push_back(std::move(entry));
  }

  PLUGIN_INFO("bridge", "Added message retry: {} -> {} (msg_id: {})",
              source_platform, target_platform, source_message_id);
}

void RetryQueueManager::add_media_download_retry(
    const std::string &platform, const std::string &file_id,
    const std::string &file_type, const std::string &download_url,
    const std::string &local_path, bool use_proxy, int max_retries,
    const std::string &failure_reason) {

  MediaDownloadRetryEntry entry;
  entry.platform = platform;
  entry.file_id = file_id;
  entry.file_type = file_type;
  entry.download_url = download_url;
  entry.local_path = local_path;
  entry.use_proxy = use_proxy;
  entry.retry_count = 0;
  entry.max_retry_count = max_retries;
  entry.failure_reason = failure_reason;
  entry.created_at = std::chrono::system_clock::now();
  entry.next_retry_at =
      calculate_next_retry_time(0, DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS);

  {
    std::lock_guard<std::mutex> lock(media_retry_mutex_);
    media_retry_queue_.push_back(std::move(entry));
  }

  PLUGIN_INFO("bridge",
              "Added media download retry: {} (file_id: {}, use_proxy: {})",
              platform, file_id, use_proxy);
}

void RetryQueueManager::register_message_send_callback(
    const std::string &target_platform, MessageSendCallback callback) {
  message_send_callbacks_[target_platform] = std::move(callback);
  PLUGIN_DEBUG("bridge", "Registered message send callback for platform: {}",
               target_platform);
}

void RetryQueueManager::register_media_download_callback(
    const std::string &platform, MediaDownloadCallback callback) {
  media_download_callbacks_[platform] = std::move(callback);
  PLUGIN_DEBUG("bridge", "Registered media download callback for platform: {}",
               platform);
}

auto RetryQueueManager::process_retry_queues() -> boost::asio::awaitable<void> {
  while (running_) {
    try {
      // Process message retries
      co_await process_message_retries();

      // Process media download retries
      co_await process_media_download_retries();

      // Wait for next check interval
      retry_timer_->expires_after(
          std::chrono::seconds(RETRY_QUEUE_CHECK_INTERVAL_SECONDS));
      co_await retry_timer_->async_wait(boost::asio::use_awaitable);

    } catch (const boost::system::system_error &e) {
      if (e.code() == boost::asio::error::operation_aborted) {
        PLUGIN_INFO("bridge", "Retry queue processing cancelled");
        break;
      }
      PLUGIN_ERROR("bridge", "Error in retry queue processing: {}", e.what());
    } catch (const std::exception &e) {
      PLUGIN_ERROR("bridge", "Exception in retry queue processing: {}",
                   e.what());
    }

    // Brief wait before continuing on error
    if (running_) {
      try {
        retry_timer_->expires_after(std::chrono::seconds(5));
        co_await retry_timer_->async_wait(boost::asio::use_awaitable);
      } catch (const boost::system::system_error &) {
        break;
      }
    }
  }

  PLUGIN_INFO("bridge", "Retry queue processing stopped");
}

auto RetryQueueManager::process_message_retries()
    -> boost::asio::awaitable<void> {
  auto now = std::chrono::system_clock::now();

  // Get entries ready for retry
  std::vector<MessageRetryEntry> ready_entries;
  {
    std::lock_guard<std::mutex> lock(message_retry_mutex_);
    for (auto it = message_retry_queue_.begin();
         it != message_retry_queue_.end();) {
      if (it->next_retry_at <= now) {
        ready_entries.push_back(std::move(*it));
        it = message_retry_queue_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (ready_entries.empty()) {
    co_return;
  }

  PLUGIN_DEBUG("bridge", "Processing {} message retries", ready_entries.size());

  for (auto &entry : ready_entries) {
    try {
      // Find callback for target platform
      auto callback_it = message_send_callbacks_.find(entry.target_platform);
      if (callback_it == message_send_callbacks_.end()) {
        PLUGIN_WARN("bridge", "No callback registered for target platform: {}",
                    entry.target_platform);
        // Put back in queue for later
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS);
        std::lock_guard<std::mutex> lock(message_retry_mutex_);
        message_retry_queue_.push_back(std::move(entry));
        continue;
      }

      // Try to send message
      PLUGIN_INFO("bridge", "Retrying message send: {} -> {} (attempt {})",
                  entry.source_platform, entry.target_platform,
                  entry.retry_count + 1);

      auto result = co_await callback_it->second(entry, entry.message);

      if (result.has_value()) {
        // Success - don't re-add to queue
        PLUGIN_INFO("bridge", "Message retry successful: {} -> {} (msg_id: {})",
                    entry.source_platform, entry.target_platform,
                    result.value());
      } else {
        // Failed - check if we should retry again
        entry.retry_count++;

        if (entry.retry_count >= entry.max_retry_count) {
          PLUGIN_WARN("bridge",
                      "Message retry failed after {} attempts: {} -> {}",
                      entry.max_retry_count, entry.source_platform,
                      entry.target_platform);
          // Don't re-add - give up
        } else {
          // Re-add with updated retry time
          entry.next_retry_at = calculate_next_retry_time(
              entry.retry_count, DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS);
          PLUGIN_DEBUG("bridge",
                       "Updated message retry count to {}, next retry in {}s",
                       entry.retry_count,
                       std::chrono::duration_cast<std::chrono::seconds>(
                           entry.next_retry_at - now)
                           .count());

          std::lock_guard<std::mutex> lock(message_retry_mutex_);
          message_retry_queue_.push_back(std::move(entry));
        }
      }

    } catch (const std::exception &e) {
      PLUGIN_ERROR("bridge", "Error processing message retry: {}", e.what());
      // Re-add to queue on error
      entry.retry_count++;
      if (entry.retry_count < entry.max_retry_count) {
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS);
        std::lock_guard<std::mutex> lock(message_retry_mutex_);
        message_retry_queue_.push_back(std::move(entry));
      }
    }
  }
}

boost::asio::awaitable<void>
RetryQueueManager::process_media_download_retries() {
  auto now = std::chrono::system_clock::now();

  // Get entries ready for retry
  std::vector<MediaDownloadRetryEntry> ready_entries;
  {
    std::lock_guard<std::mutex> lock(media_retry_mutex_);
    for (auto it = media_retry_queue_.begin();
         it != media_retry_queue_.end();) {
      if (it->next_retry_at <= now) {
        ready_entries.push_back(std::move(*it));
        it = media_retry_queue_.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (ready_entries.empty()) {
    co_return;
  }

  PLUGIN_DEBUG("bridge", "Processing {} media download retries",
               ready_entries.size());

  for (auto &entry : ready_entries) {
    try {
      auto callback_it = media_download_callbacks_.find(entry.platform);
      if (callback_it == media_download_callbacks_.end()) {
        PLUGIN_WARN("bridge", "No callback registered for platform: {}",
                    entry.platform);
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS);
        std::lock_guard<std::mutex> lock(media_retry_mutex_);
        media_retry_queue_.push_back(std::move(entry));
        continue;
      }

      PLUGIN_INFO("bridge",
                  "Retrying media download: {} (attempt {}, use_proxy: {})",
                  entry.file_id, entry.retry_count + 1, entry.use_proxy);

      auto result = co_await callback_it->second(
          entry.download_url, entry.local_path, entry.use_proxy);

      if (result.has_value()) {
        PLUGIN_INFO("bridge", "Media download retry successful: {} -> {}",
                    entry.file_id, result.value());
      } else {
        entry.retry_count++;

        if (entry.retry_count >= entry.max_retry_count) {
          // Try direct connection if proxy failed
          if (entry.use_proxy) {
            PLUGIN_INFO("bridge",
                        "Proxy download failed, trying direct connection: {}",
                        entry.file_id);
            entry.use_proxy = false;
            entry.retry_count = 0; // Reset count for direct connection
            entry.next_retry_at = calculate_next_retry_time(
                0, DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS);
            std::lock_guard<std::mutex> lock(media_retry_mutex_);
            media_retry_queue_.push_back(std::move(entry));
          } else {
            PLUGIN_WARN("bridge",
                        "Media download retry failed after {} attempts: {}",
                        entry.max_retry_count, entry.file_id);
          }
        } else {
          entry.next_retry_at = calculate_next_retry_time(
              entry.retry_count, DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS);
          std::lock_guard<std::mutex> lock(media_retry_mutex_);
          media_retry_queue_.push_back(std::move(entry));
        }
      }

    } catch (const std::exception &e) {
      PLUGIN_ERROR("bridge", "Error processing media download retry: {}",
                   e.what());
      entry.retry_count++;
      if (entry.retry_count < entry.max_retry_count) {
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS);
        std::lock_guard<std::mutex> lock(media_retry_mutex_);
        media_retry_queue_.push_back(std::move(entry));
      }
    }
  }
}

std::chrono::system_clock::time_point
RetryQueueManager::calculate_next_retry_time(int retry_count,
                                             int base_interval_seconds) const {
  // Exponential backoff: 2^retry_count * base_interval, with max limit
  int delay_seconds =
      static_cast<int>(std::pow(2, retry_count)) * base_interval_seconds;
  delay_seconds = std::min(delay_seconds, MAX_RETRY_INTERVAL_SECONDS);

  return std::chrono::system_clock::now() + std::chrono::seconds(delay_seconds);
}

size_t RetryQueueManager::get_pending_message_retry_count() const {
  std::lock_guard<std::mutex> lock(message_retry_mutex_);
  return message_retry_queue_.size();
}

size_t RetryQueueManager::get_pending_media_retry_count() const {
  std::lock_guard<std::mutex> lock(media_retry_mutex_);
  return media_retry_queue_.size();
}

std::string RetryQueueManager::get_retry_statistics() const {
  std::ostringstream stats;

  size_t msg_count = get_pending_message_retry_count();
  size_t media_count = get_pending_media_retry_count();

  stats << "=== Retry Queue Statistics (In-Memory) ===\n";
  stats << "Pending message retries: " << msg_count << "\n";
  stats << "Pending media download retries: " << media_count << "\n";

  return stats.str();
}

} // namespace bridge

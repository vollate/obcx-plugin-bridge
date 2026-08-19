#include "retry_queue_manager.hpp"
#include "bridge_state_repository.hpp"

#include <algorithm>
#include <common/logger.hpp>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace bridge {
namespace {

auto serialize_message(const obcx::common::Message &message) -> std::string {
  nlohmann::json json_message = nlohmann::json::array();
  for (const auto &segment : message) {
    json_message.push_back({{"type", segment.type}, {"data", segment.data}});
  }
  return json_message.dump();
}

auto deserialize_message(const std::string &payload) -> obcx::common::Message {
  obcx::common::Message message;
  if (payload.empty()) {
    return message;
  }

  const auto json_message = nlohmann::json::parse(payload);
  if (!json_message.is_array()) {
    return message;
  }

  for (const auto &item : json_message) {
    obcx::common::MessageSegment segment;
    segment.type = item.value("type", std::string{});
    if (item.contains("data") && item["data"].is_object()) {
      segment.data = item["data"];
    } else {
      segment.data = nlohmann::json::object();
    }
    message.push_back(std::move(segment));
  }
  return message;
}

auto to_retry_info(const MessageRetryEntry &entry)
    -> storage::MessageRetryInfo {
  storage::MessageRetryInfo retry_info;
  retry_info.source_platform = entry.source_platform;
  retry_info.target_platform = entry.target_platform;
  retry_info.source_message_id = entry.source_message_id;
  retry_info.message_content = serialize_message(entry.message);
  retry_info.group_id = entry.group_id;
  retry_info.source_group_id = entry.source_group_id;
  retry_info.target_topic_id = entry.target_topic_id;
  retry_info.retry_count = entry.retry_count;
  retry_info.max_retry_count = entry.max_retry_count;
  retry_info.failure_reason = entry.failure_reason;
  retry_info.retry_type = "message_send";
  retry_info.next_retry_at = entry.next_retry_at;
  retry_info.created_at = entry.created_at;
  retry_info.last_attempt_at = std::chrono::system_clock::now();
  return retry_info;
}

auto to_retry_entry(const storage::MessageRetryInfo &retry_info)
    -> MessageRetryEntry {
  MessageRetryEntry entry;
  entry.source_platform = retry_info.source_platform;
  entry.target_platform = retry_info.target_platform;
  entry.source_message_id = retry_info.source_message_id;
  entry.message = deserialize_message(retry_info.message_content);
  entry.group_id = retry_info.group_id;
  entry.source_group_id = retry_info.source_group_id;
  entry.target_topic_id = retry_info.target_topic_id;
  entry.retry_count = retry_info.retry_count;
  entry.max_retry_count = retry_info.max_retry_count;
  entry.failure_reason = retry_info.failure_reason;
  entry.next_retry_at = retry_info.next_retry_at;
  entry.created_at = retry_info.created_at;
  return entry;
}

auto remove_persisted_message_retry(
    const std::shared_ptr<BridgeStateRepository> &repository,
    const MessageRetryEntry &entry, const char *outcome) -> bool {
  if (!repository) {
    return true;
  }

  try {
    return repository->remove_message_retry(
        entry.source_platform, entry.source_message_id, entry.target_platform);
  } catch (const std::exception &e) {
    OBCX_ERROR("Failed to remove persisted message retry after {}: {}", outcome,
               e.what());
    return false;
  }
}

auto update_persisted_message_retry(
    const std::shared_ptr<BridgeStateRepository> &repository,
    const MessageRetryEntry &entry, const char *outcome) -> bool {
  if (!repository) {
    return true;
  }

  try {
    return repository->update_message_retry(
        entry.source_platform, entry.source_message_id, entry.target_platform,
        entry.retry_count, entry.next_retry_at, entry.failure_reason);
  } catch (const std::exception &e) {
    OBCX_ERROR("Failed to update persisted message retry after {}: {}", outcome,
               e.what());
    return false;
  }
}

auto terminalize_persisted_message_retry(
    const std::shared_ptr<BridgeStateRepository> &repository,
    MessageRetryEntry &entry, std::string reason) -> bool {
  entry.retry_count = entry.max_retry_count;
  entry.failure_reason = std::move(reason);
  entry.next_retry_at = std::chrono::system_clock::now();
  return update_persisted_message_retry(repository, entry, "terminal outcome");
}

auto persist_successful_message_mapping(
    const std::shared_ptr<BridgeStateRepository> &repository,
    const MessageRetryEntry &entry, const std::string &target_message_id)
    -> bool {
  if (!repository) {
    return true;
  }
  if (target_message_id.empty()) {
    return false;
  }

  try {
    storage::MessageMapping mapping;
    mapping.source_platform = entry.source_platform;
    mapping.source_message_id = entry.source_message_id;
    mapping.target_platform = entry.target_platform;
    mapping.target_message_id = target_message_id;
    mapping.created_at = std::chrono::system_clock::now();
    return repository->add_message_mapping(
        mapping, MessageMappingWritePurpose::RetryCompletion);
  } catch (const std::exception &e) {
    OBCX_ERROR("Failed to persist mapping after successful retry: {}",
               e.what());
    return false;
  }
}

auto same_retry_identity(const MessageRetryEntry &left,
                         const MessageRetryEntry &right) -> bool {
  return left.source_platform == right.source_platform &&
         left.source_message_id == right.source_message_id &&
         left.target_platform == right.target_platform;
}

} // namespace

RetryQueueManager::RetryQueueManager(boost::asio::io_context &io_context,
                                     RetryQueuePolicy policy)
    : io_context_(io_context), policy_(policy),
      retry_timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
      running_(false), stopped_future_(stopped_promise_.get_future().share()) {
  OBCX_INFO("RetryQueueManager initialized (in-memory mode)");
}

RetryQueueManager::RetryQueueManager(
    boost::asio::io_context &io_context,
    std::shared_ptr<BridgeStateRepository> state_repository,
    RetryQueuePolicy policy)
    : io_context_(io_context), state_repository_(std::move(state_repository)),
      policy_(policy),
      retry_timer_(std::make_unique<boost::asio::steady_timer>(io_context)),
      running_(false), stopped_future_(stopped_promise_.get_future().share()) {
  OBCX_INFO("RetryQueueManager initialized (persistent mode)");
}

RetryQueueManager::~RetryQueueManager() { stop(); }

void RetryQueueManager::start() {

  bool expect_running_ = false;
  if (!running_.compare_exchange_strong(expect_running_, true)) {
    OBCX_WARN("RetryQueueManager already running");
    return;
  }

  OBCX_INFO("Starting RetryQueueManager");

  // Keep the manager alive while its detached coroutine is suspended in an
  // in-flight retry callback. Shutdown can otherwise destroy `this` before a
  // timed-out send resumes.
  if (auto self = weak_from_this().lock()) {
    boost::asio::co_spawn(
        io_context_,
        [self]() -> boost::asio::awaitable<void> {
          co_await self->process_retry_queues();
        },
        [self](std::exception_ptr error) {
          if (error) {
            try {
              std::rethrow_exception(error);
            } catch (const std::exception &exception) {
              OBCX_ERROR("Retry queue terminated with an exception: {}",
                         exception.what());
            } catch (...) {
              OBCX_ERROR("Retry queue terminated with an unknown exception");
            }
          }
          self->signal_stopped();
        });
  } else {
    boost::asio::co_spawn(
        io_context_, process_retry_queues(), [this](std::exception_ptr error) {
          if (error) {
            OBCX_ERROR("Retry queue terminated with an exception");
          }
          signal_stopped();
        });
  }
}

void RetryQueueManager::stop() {
  if (!running_) {
    return;
  }

  OBCX_INFO("Stopping RetryQueueManager");
  running_ = false;

  if (retry_timer_) {
    retry_timer_->cancel();
  }

  {
    std::scoped_lock lock(message_retry_mutex_);
    size_t msg_count = message_retry_queue_.size();
    message_retry_queue_.clear();
    if (msg_count > 0) {
      OBCX_INFO("Cleared {} pending message retries", msg_count);
    }
  }
  {
    std::scoped_lock lock(media_retry_mutex_);
    size_t media_count = media_retry_queue_.size();
    media_retry_queue_.clear();
    if (media_count > 0) {
      OBCX_INFO("Cleared {} pending media download retries", media_count);
    }
  }
}

auto RetryQueueManager::stopped() const -> std::shared_future<void> {
  return stopped_future_;
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
  entry.message = message;
  entry.group_id = group_id;
  entry.source_group_id = source_group_id;
  entry.target_topic_id = target_topic_id;
  entry.retry_count = 0;
  entry.max_retry_count = max_retries;
  entry.failure_reason = failure_reason;
  entry.created_at = std::chrono::system_clock::now();
  entry.next_retry_at =
      calculate_next_retry_time(0, policy_.message_retry_base_interval_sec);

  const auto retry_info = to_retry_info(entry);

  {
    std::scoped_lock lock(message_retry_mutex_);
    const auto existing = std::ranges::find_if(
        message_retry_queue_, [&entry](const MessageRetryEntry &candidate) {
          return same_retry_identity(candidate, entry);
        });
    if (existing == message_retry_queue_.end()) {
      message_retry_queue_.push_back(entry);
    } else {
      *existing = entry;
    }
  }

  if (state_repository_) {
    try {
      state_repository_->add_message_retry(retry_info);
    } catch (const std::exception &e) {
      OBCX_ERROR("Failed to persist message retry: {}", e.what());
    }
  }

  OBCX_INFO("Added message retry: {} -> {} (msg_id: {})", source_platform,
            target_platform, source_message_id);
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
      calculate_next_retry_time(0, policy_.media_retry_base_interval_sec);

  {
    std::scoped_lock lock(media_retry_mutex_);
    media_retry_queue_.push_back(std::move(entry));
  }

  OBCX_INFO("Added media download retry: {} (file_id: {}, use_proxy: {})",
            platform, file_id, use_proxy);
}

void RetryQueueManager::register_message_send_callback(
    const std::string &target_platform, MessageSendCallback callback) {
  message_send_callbacks_[target_platform] = std::move(callback);
  OBCX_DEBUG("Registered message send callback for platform: {}",
             target_platform);
}

void RetryQueueManager::register_media_download_callback(
    const std::string &platform, MediaDownloadCallback callback) {
  media_download_callbacks_[platform] = std::move(callback);
  OBCX_DEBUG("Registered media download callback for platform: {}", platform);
}

auto RetryQueueManager::process_retry_queues() -> boost::asio::awaitable<void> {
  while (running_) {
    try {
      co_await process_message_retries();
      if (!running_) {
        break;
      }

      co_await process_media_download_retries();
      if (!running_) {
        break;
      }

      retry_timer_->expires_after(
          std::chrono::seconds(policy_.retry_queue_check_interval_sec));
      co_await retry_timer_->async_wait(boost::asio::use_awaitable);

    } catch (const boost::system::system_error &e) {
      if (e.code() == boost::asio::error::operation_aborted) {
        OBCX_INFO("Retry queue processing cancelled");
        break;
      }
      OBCX_ERROR("Error in retry queue processing: {}", e.what());
    } catch (const std::exception &e) {
      OBCX_ERROR("Exception in retry queue processing: {}", e.what());
    }

    if (running_) {
      try {
        retry_timer_->expires_after(
            std::chrono::seconds(policy_.retry_queue_check_interval_sec));
        co_await retry_timer_->async_wait(boost::asio::use_awaitable);
      } catch (const boost::system::system_error &) {
        break;
      }
    }
  }

  OBCX_INFO("Retry queue processing stopped");
}

auto RetryQueueManager::process_message_retries()
    -> boost::asio::awaitable<void> {
  auto now = std::chrono::system_clock::now();

  std::vector<MessageRetryEntry> ready_entries;
  {
    std::scoped_lock lock(message_retry_mutex_);
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

  OBCX_DEBUG("Processing {} message retries", ready_entries.size());

  for (auto &entry : ready_entries) {
    try {
      auto callback_it = message_send_callbacks_.find(entry.target_platform);
      if (callback_it == message_send_callbacks_.end()) {
        OBCX_WARN("No callback registered for target platform: {}",
                  entry.target_platform);
        // No handler yet - keep entry alive so it can be retried later
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, policy_.message_retry_base_interval_sec);
        update_persisted_message_retry(state_repository_, entry,
                                       "missing callback");
        std::scoped_lock lock(message_retry_mutex_);
        message_retry_queue_.push_back(std::move(entry));
        continue;
      }

      OBCX_INFO("Retrying message send: {} -> {} (attempt {})",
                entry.source_platform, entry.target_platform,
                entry.retry_count + 1);

      if (state_repository_) {
        const auto mapped = state_repository_->get_target_message_id(
            entry.source_platform, entry.source_message_id,
            entry.target_platform);
        if (mapped.has_value()) {
          if (remove_persisted_message_retry(state_repository_, entry,
                                             "existing mapping")) {
            OBCX_INFO("Removed completed retry with existing mapping: {} -> {}",
                      entry.source_platform, entry.target_platform);
          } else if (running_) {
            entry.failure_reason = "retry row cleanup failed";
            entry.next_retry_at = calculate_next_retry_time(
                entry.retry_count, policy_.message_retry_base_interval_sec);
            std::scoped_lock lock(message_retry_mutex_);
            message_retry_queue_.push_back(std::move(entry));
          }
          continue;
        }
      }

      auto result = co_await callback_it->second(entry, entry.message);

      if (result.disposition == MessageSendDisposition::Completed) {
        if (!result.target_message_id.has_value() ||
            result.target_message_id->empty()) {
          terminalize_persisted_message_retry(
              state_repository_, entry, "completed result missing message id");
          continue;
        }
        const bool mapping_persisted = persist_successful_message_mapping(
            state_repository_, entry, *result.target_message_id);
        const bool retry_removed =
            mapping_persisted &&
            remove_persisted_message_retry(state_repository_, entry, "success");
        if (mapping_persisted && retry_removed) {
          OBCX_INFO("Message retry successful: {} -> {} (msg_id: {})",
                    entry.source_platform, entry.target_platform,
                    *result.target_message_id);
        } else if (mapping_persisted && running_) {
          // The mapping makes a future pre-send check safe; retain only for
          // cleanup recovery and never submit before checking that mapping.
          entry.failure_reason = "retry row cleanup failed";
          entry.next_retry_at = calculate_next_retry_time(
              entry.retry_count, policy_.message_retry_base_interval_sec);
          update_persisted_message_retry(state_repository_, entry,
                                         "completion cleanup failure");
          std::scoped_lock lock(message_retry_mutex_);
          message_retry_queue_.push_back(std::move(entry));
        } else {
          // Provider completion without a durable mapping cannot be retried
          // safely. Keep the existing row terminal without changing schema.
          terminalize_persisted_message_retry(
              state_repository_, entry, "retry mapping persistence failed");
        }
      } else if (result.disposition ==
                 MessageSendDisposition::RetryableFailure) {
        entry.retry_count++;
        entry.failure_reason = result.diagnostic.empty()
                                   ? "definite retryable failure"
                                   : result.diagnostic;
        if (entry.retry_count >= entry.max_retry_count) {
          OBCX_WARN("Message retry failed after {} attempts: {} -> {}",
                    entry.max_retry_count, entry.source_platform,
                    entry.target_platform);
          remove_persisted_message_retry(state_repository_, entry,
                                         "max attempts");
        } else {
          entry.next_retry_at = calculate_next_retry_time(
              entry.retry_count, policy_.message_retry_base_interval_sec);
          update_persisted_message_retry(state_repository_, entry,
                                         "definite send failure");
          if (running_) {
            std::scoped_lock lock(message_retry_mutex_);
            message_retry_queue_.push_back(std::move(entry));
          }
        }
      } else {
        const std::string reason =
            result.disposition == MessageSendDisposition::OutcomeUnknown
                ? "outcome_unknown"
                : "terminal_failure";
        terminalize_persisted_message_retry(
            state_repository_, entry,
            result.diagnostic.empty() ? reason
                                      : reason + ":" + result.diagnostic);
        OBCX_WARN("Message retry stopped with {}: {} -> {}", reason,
                  entry.source_platform, entry.target_platform);
      }

    } catch (const std::exception &e) {
      OBCX_ERROR("Error processing message retry: {}", e.what());
      terminalize_persisted_message_retry(state_repository_, entry,
                                          "outcome_unknown:callback_exception");
    } catch (...) {
      OBCX_ERROR("Unknown error processing message retry");
      terminalize_persisted_message_retry(state_repository_, entry,
                                          "outcome_unknown:unknown_exception");
    }
  }
}

auto RetryQueueManager::process_media_download_retries()
    -> boost::asio::awaitable<void> {
  auto now = std::chrono::system_clock::now();

  std::vector<MediaDownloadRetryEntry> ready_entries;
  {
    std::scoped_lock lock(media_retry_mutex_);
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

  OBCX_DEBUG("Processing {} media download retries", ready_entries.size());

  for (auto &entry : ready_entries) {
    try {
      auto callback_it = media_download_callbacks_.find(entry.platform);
      if (callback_it == media_download_callbacks_.end()) {
        OBCX_WARN("No callback registered for platform: {}", entry.platform);
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, policy_.media_retry_base_interval_sec);
        std::scoped_lock lock(media_retry_mutex_);
        media_retry_queue_.push_back(std::move(entry));
        continue;
      }

      OBCX_INFO("Retrying media download: {} (attempt {}, use_proxy: {})",
                entry.file_id, entry.retry_count + 1, entry.use_proxy);

      auto result = co_await callback_it->second(
          entry.download_url, entry.local_path, entry.use_proxy);

      if (result.has_value()) {
        OBCX_INFO("Media download retry successful: {} -> {}", entry.file_id,
                  result.value());
      } else {
        entry.retry_count++;

        if (entry.retry_count >= entry.max_retry_count) {
          // Proxy exhausted: fall back to direct connection with fresh count
          if (running_ && entry.use_proxy) {
            OBCX_INFO("Proxy download failed, trying direct connection: {}",
                      entry.file_id);
            entry.use_proxy = false;
            entry.retry_count = 0;
            entry.next_retry_at = calculate_next_retry_time(
                0, policy_.media_retry_base_interval_sec);
            std::scoped_lock lock(media_retry_mutex_);
            media_retry_queue_.push_back(std::move(entry));
          } else {
            OBCX_WARN("Media download retry failed after {} attempts: {}",
                      entry.max_retry_count, entry.file_id);
          }
        } else if (running_) {
          entry.next_retry_at = calculate_next_retry_time(
              entry.retry_count, policy_.media_retry_base_interval_sec);
          std::scoped_lock lock(media_retry_mutex_);
          media_retry_queue_.push_back(std::move(entry));
        }
      }

    } catch (const std::exception &e) {
      OBCX_ERROR("Error processing media download retry: {}", e.what());
      entry.retry_count++;
      if (running_ && entry.retry_count < entry.max_retry_count) {
        entry.next_retry_at = calculate_next_retry_time(
            entry.retry_count, policy_.media_retry_base_interval_sec);
        std::scoped_lock lock(media_retry_mutex_);
        media_retry_queue_.push_back(std::move(entry));
      }
    }
  }
}

auto RetryQueueManager::calculate_next_retry_time(
    int retry_count, int base_interval_seconds) const
    -> std::chrono::system_clock::time_point {
  const auto safe_retry_count = std::max(0, retry_count);
  std::int64_t delay_seconds = base_interval_seconds;
  for (int attempt = 0; attempt < safe_retry_count &&
                        delay_seconds < policy_.max_retry_interval_sec;
       ++attempt) {
    delay_seconds = std::min<std::int64_t>(delay_seconds * 2,
                                           policy_.max_retry_interval_sec);
  }
  delay_seconds = std::clamp<std::int64_t>(delay_seconds, 1,
                                           policy_.max_retry_interval_sec);

  return std::chrono::system_clock::now() + std::chrono::seconds(delay_seconds);
}

auto RetryQueueManager::get_pending_message_retry_count() const -> size_t {
  std::scoped_lock lock(message_retry_mutex_);
  return message_retry_queue_.size();
}

auto RetryQueueManager::get_pending_media_retry_count() const -> size_t {
  std::scoped_lock lock(media_retry_mutex_);
  return media_retry_queue_.size();
}

void RetryQueueManager::restore_persisted_message_retries() {
  if (!state_repository_) {
    return;
  }

  auto retries = state_repository_->get_pending_message_retries(
      std::chrono::system_clock::time_point::max(), 10000);

  std::deque<MessageRetryEntry> restored;
  for (const auto &retry : retries) {
    restored.push_back(to_retry_entry(retry));
  }

  std::scoped_lock lock(message_retry_mutex_);
  message_retry_queue_ = std::move(restored);
  OBCX_INFO("Restored {} persisted message retries",
            message_retry_queue_.size());
}

auto RetryQueueManager::get_retry_statistics() const -> std::string {
  std::ostringstream stats;

  size_t msg_count = get_pending_message_retry_count();
  size_t media_count = get_pending_media_retry_count();

  stats << "=== Retry Queue Statistics (In-Memory) ===\n";
  stats << "Pending message retries: " << msg_count << "\n";
  stats << "Pending media download retries: " << media_count << "\n";

  return stats.str();
}

void RetryQueueManager::signal_stopped() noexcept {
  bool expected = false;
  if (!stopped_signalled_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
    return;
  }
  try {
    stopped_promise_.set_value();
  } catch (...) {
  }
}

} // namespace bridge

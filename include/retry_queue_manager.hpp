#pragma once

#include "common/message_type.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bridge {

/**
 * @brief In-memory message retry info (no database persistence)
 */
struct MessageRetryEntry {
  std::string source_platform;
  std::string target_platform;
  std::string source_message_id;
  obcx::common::Message message; // Store message directly, no serialization
  std::string group_id;
  std::string source_group_id;
  int64_t target_topic_id;
  int retry_count;
  int max_retry_count;
  std::string failure_reason;
  std::chrono::system_clock::time_point next_retry_at;
  std::chrono::system_clock::time_point created_at;
};

/**
 * @brief In-memory media download retry info (no database persistence)
 */
struct MediaDownloadRetryEntry {
  std::string platform;
  std::string file_id;
  std::string file_type;
  std::string download_url;
  std::string local_path;
  bool use_proxy;
  int retry_count;
  int max_retry_count;
  std::string failure_reason;
  std::chrono::system_clock::time_point next_retry_at;
  std::chrono::system_clock::time_point created_at;
};

/**
 * @brief 重试队列管理器 (In-memory, non-persistent)
 *
 * 负责管理消息发送重试和媒体下载重试的队列处理
 * 实现指数退避算法，避免频繁重试导致的系统压力
 * 所有数据存储在内存中，重启后清空
 */
class RetryQueueManager {
public:
  using MessageSendCallback =
      std::function<boost::asio::awaitable<std::optional<std::string>>(
          const MessageRetryEntry &retry_info,
          const obcx::common::Message &message)>;

  using MediaDownloadCallback =
      std::function<boost::asio::awaitable<std::optional<std::string>>(
          const std::string &download_url, const std::string &local_path,
          bool use_proxy)>;

  /**
   * @brief 构造函数
   * @param io_context ASIO IO上下文
   */
  explicit RetryQueueManager(boost::asio::io_context &io_context);

  /**
   * @brief 析构函数
   */
  ~RetryQueueManager();

  /**
   * @brief 启动重试队列处理
   */
  void start();

  /**
   * @brief 停止重试队列处理
   */
  void stop();

  /**
   * @brief 添加消息发送重试
   */
  void add_message_retry(const std::string &source_platform,
                         const std::string &target_platform,
                         const std::string &source_message_id,
                         const obcx::common::Message &message,
                         const std::string &group_id,
                         const std::string &source_group_id,
                         int64_t target_topic_id, int max_retries = 5,
                         const std::string &failure_reason = "");

  /**
   * @brief 添加媒体下载重试
   */
  void add_media_download_retry(const std::string &platform,
                                const std::string &file_id,
                                const std::string &file_type,
                                const std::string &download_url,
                                const std::string &local_path,
                                bool use_proxy = true, int max_retries = 3,
                                const std::string &failure_reason = "");

  /**
   * @brief 注册消息发送回调函数
   */
  void register_message_send_callback(const std::string &target_platform,
                                      MessageSendCallback callback);

  /**
   * @brief 注册媒体下载回调函数
   */
  void register_media_download_callback(const std::string &platform,
                                        MediaDownloadCallback callback);

  /**
   * @brief 获取重试统计信息
   */
  auto get_retry_statistics() const -> std::string;

  /**
   * @brief 获取待处理消息重试数量
   */
  auto get_pending_message_retry_count() const -> size_t;

  /**
   * @brief 获取待处理媒体下载重试数量
   */
  auto get_pending_media_retry_count() const -> size_t;

private:
  boost::asio::io_context &io_context_;
  std::unique_ptr<boost::asio::steady_timer> retry_timer_;
  std::atomic_bool running_;

  // In-memory retry queues (thread-safe)
  mutable std::mutex message_retry_mutex_;
  mutable std::mutex media_retry_mutex_;
  std::deque<MessageRetryEntry> message_retry_queue_;
  std::deque<MediaDownloadRetryEntry> media_retry_queue_;

  // Callback mappings
  std::unordered_map<std::string, MessageSendCallback> message_send_callbacks_;
  std::unordered_map<std::string, MediaDownloadCallback>
      media_download_callbacks_;

  // Retry configuration
  static constexpr int DEFAULT_MESSAGE_RETRY_INTERVAL_SECONDS = 2;
  static constexpr int DEFAULT_MEDIA_RETRY_INTERVAL_SECONDS = 5;
  static constexpr int MAX_RETRY_INTERVAL_SECONDS = 300; // 5 minutes
  static constexpr int RETRY_QUEUE_CHECK_INTERVAL_SECONDS = 10;

  /**
   * @brief 定期检查重试队列
   */
  auto process_retry_queues() -> boost::asio::awaitable<void>;

  /**
   * @brief 处理消息发送重试
   */
  auto process_message_retries() -> boost::asio::awaitable<void>;

  /**
   * @brief 处理媒体下载重试
   */
  auto process_media_download_retries() -> boost::asio::awaitable<void>;

  /**
   * @brief 计算下次重试时间（指数退避）
   */
  auto calculate_next_retry_time(int retry_count,
                                 int base_interval_seconds) const
      -> std::chrono::system_clock::time_point;
};

} // namespace bridge

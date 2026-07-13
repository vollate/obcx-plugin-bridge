#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <common/message_type.hpp>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bridge::telegram {

/**
 * @brief Buffer for Telegram media-group (album) messages.
 *
 * Telegram delivers a media-group as several distinct Update events that share
 * the same `media_group_id` field. Forwarding each one independently would
 * produce N separate QQ messages instead of a single message containing every
 * image. This buffer collects events keyed by (chat_id, media_group_id) and,
 * after a short debounce window in which no new events arrive, calls back the
 * supplied flush function with the full batch so the caller can build a single
 * combined QQ message.
 *
 * Thread-safe.
 */
class TGMediaGroupBuffer
    : public std::enable_shared_from_this<TGMediaGroupBuffer> {
public:
  using FlushFn = std::function<void(std::vector<obcx::common::MessageEvent>)>;

  /// Debounce window: how long to wait for additional messages in a group
  /// before flushing. Telegram sends the album updates back-to-back, so 1.5s
  /// is comfortably above any inter-message gap while keeping perceived
  /// latency low.
  static constexpr std::chrono::milliseconds kDebounceWindow{1500};

  explicit TGMediaGroupBuffer(boost::asio::any_io_executor executor);
  ~TGMediaGroupBuffer();

  TGMediaGroupBuffer(const TGMediaGroupBuffer &) = delete;
  auto operator=(const TGMediaGroupBuffer &) -> TGMediaGroupBuffer & = delete;

  /**
   * @brief Append an event to the buffer for its (chat, media_group_id) key.
   *
   * Each call resets the debounce timer for that group. When the timer fires,
   * `flush_fn` is invoked once with all events accumulated so far.
   */
  void add(obcx::common::MessageEvent event, FlushFn flush_fn);

  /**
   * @brief Flush all currently buffered groups synchronously.
   *
   * Called during actor shutdown to ensure no group is silently dropped if
   * its debounce timer was still pending. Each group's stored callback is
   * invoked exactly once on the calling thread.
   */
  void flush_all_now();

private:
  using GroupKey = std::pair<std::string, std::string>; // (chat_id, mgid)

  struct Group {
    std::vector<obcx::common::MessageEvent> events;
    FlushFn flush_fn;
    std::unique_ptr<boost::asio::steady_timer> timer;
  };

  void on_timer_fired_(GroupKey key);

  boost::asio::any_io_executor executor_;
  mutable std::mutex mutex_;
  std::map<GroupKey, Group> groups_;
};

} // namespace bridge::telegram

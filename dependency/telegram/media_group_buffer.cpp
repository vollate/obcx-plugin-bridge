#include "telegram/media_group_buffer.hpp"

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <common/logger.hpp>
#include <utility>

namespace bridge::telegram {

namespace {

/// Pull the media_group_id out of the raw Telegram update stored on
/// MessageEvent.data. obcx_core's `common::MessageEvent` does not carry the
/// field directly, so we read it from the unparsed update payload (the
/// adapter copies the whole `message` object into `event.data`). Telegram
/// transmits media_group_id as a string of digits; tolerate a numeric form
/// for robustness.
auto extract_media_group_id(const obcx::common::MessageEvent &event)
    -> std::string {
  if (!event.data.contains("media_group_id")) {
    return {};
  }
  const auto &v = event.data["media_group_id"];
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<int64_t>());
  }
  if (v.is_number_unsigned()) {
    return std::to_string(v.get<uint64_t>());
  }
  return {};
}

} // namespace

TGMediaGroupBuffer::TGMediaGroupBuffer(boost::asio::any_io_executor executor)
    : executor_(std::move(executor)) {}

TGMediaGroupBuffer::~TGMediaGroupBuffer() {
  // Defensive: the owner is expected to call flush_all_now() during shutdown
  // before destroying the buffer, but if it didn't, drain anything left so we
  // don't leak buffered events. Timers are cancelled implicitly when their
  // unique_ptr is destroyed with the map.
  try {
    flush_all_now();
  } catch (...) {
    // Destructors never throw.
  }
}

void TGMediaGroupBuffer::add(obcx::common::MessageEvent event,
                             FlushFn flush_fn) {
  const std::string mgid = extract_media_group_id(event);
  if (mgid.empty()) {
    // Caller should have checked, but handle gracefully: if there's no
    // media_group_id we can't key the buffer; flush this single event right
    // away through the callback.
    if (flush_fn) {
      std::vector<obcx::common::MessageEvent> single;
      single.push_back(std::move(event));
      flush_fn(std::move(single));
    }
    return;
  }

  GroupKey key{event.group_id.value_or(""), mgid};

  std::scoped_lock lock(mutex_);
  auto &group = groups_[key];
  group.events.push_back(std::move(event));
  // Always overwrite the callback with the latest one. The captured bot
  // pointers are stable for the actor's lifetime, so any of the calls yields
  // an equivalent functor; we just ensure the most recent capture is what
  // fires.
  group.flush_fn = std::move(flush_fn);

  if (!group.timer) {
    group.timer = std::make_unique<boost::asio::steady_timer>(executor_);
  }
  group.timer->expires_after(kDebounceWindow);

  std::weak_ptr<TGMediaGroupBuffer> weak_self = weak_from_this();
  group.timer->async_wait(
      [weak_self, key](const boost::system::error_code &ec) {
        if (ec == boost::asio::error::operation_aborted) {
          return; // Re-armed by another add(), or buffer being shut down.
        }
        auto self = weak_self.lock();
        if (!self) {
          return;
        }
        self->on_timer_fired_(key);
      });
}

void TGMediaGroupBuffer::on_timer_fired_(GroupKey key) {
  std::vector<obcx::common::MessageEvent> events;
  FlushFn fn;
  {
    std::scoped_lock lock(mutex_);
    auto it = groups_.find(key);
    if (it == groups_.end()) {
      return; // Already flushed (e.g. via flush_all_now).
    }
    events = std::move(it->second.events);
    fn = std::move(it->second.flush_fn);
    groups_.erase(it);
  }

  if (fn && !events.empty()) {
    try {
      fn(std::move(events));
    } catch (const std::exception &e) {
      OBCX_ERROR("Media group flush callback threw: {}",
                   e.what());
    }
  }
}

void TGMediaGroupBuffer::flush_all_now() {
  std::vector<std::pair<std::vector<obcx::common::MessageEvent>, FlushFn>>
      pending;
  {
    std::scoped_lock lock(mutex_);
    for (auto &kv : groups_) {
      auto &group = kv.second;
      if (group.timer) {
        // Cancel rather than destroy first, so any handler that already
        // hopped onto the io_context sees operation_aborted and returns.
        group.timer->cancel();
      }
      if (!group.events.empty()) {
        pending.emplace_back(std::move(group.events),
                             std::move(group.flush_fn));
      }
    }
    groups_.clear();
  }

  if (!pending.empty()) {
    OBCX_INFO("Flushing {} pending Telegram media groups synchronously",
                pending.size());
  }

  for (auto &[events, fn] : pending) {
    if (fn && !events.empty()) {
      try {
        fn(std::move(events));
      } catch (const std::exception &e) {
        OBCX_ERROR("Media group sync flush callback threw: {}",
                     e.what());
      }
    }
  }
}

} // namespace bridge::telegram

#include "telegram/handler.hpp"
#include "media_processor.hpp"
#include "retry_queue_manager.hpp"
#include "telegram/media_group_buffer.hpp"
#include "telegram/media_processor.hpp"
#include "telegram/message_formatter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <common/logger.hpp>
#include <config.hpp>
#include <fmt/format.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

namespace bridge {

namespace {

/// Read media_group_id from the raw Telegram update payload stored on
/// MessageEvent.data. obcx_core's `common::MessageEvent` has no first-class
/// field for it, so we extract it from the unparsed update — adding a field
/// in obcx_core is out of scope for this fix.
auto get_media_group_id(const obcx::common::MessageEvent &event)
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

TelegramHandler::TelegramHandler(
    const std::shared_ptr<storage::DatabaseManager> &db_manager,
    std::shared_ptr<RetryQueueManager> retry_manager,
    boost::asio::any_io_executor buffer_executor)
    : db_manager_(db_manager), retry_manager_(std::move(retry_manager)),
      media_processor_(
          std::make_unique<telegram::TelegramMediaProcessor>(db_manager)),
      command_handler_(
          std::make_unique<telegram::TelegramCommandHandler>(db_manager)),
      event_handler_(std::make_unique<telegram::TelegramEventHandler>(
          db_manager,
          [this](obcx::core::IBot &tg_bot, obcx::core::IBot &qq_bot,
                 obcx::common::MessageEvent event)
              -> boost::asio::awaitable<void> {
            return forward_to_qq(tg_bot, qq_bot, std::move(event));
          })),
      media_group_buffer_(
          std::make_shared<telegram::TGMediaGroupBuffer>(buffer_executor)),
      buffer_executor_(std::move(buffer_executor)) {}

auto TelegramHandler::forward_to_qq(obcx::core::IBot &telegram_bot,
                                    obcx::core::IBot &qq_bot,
                                    obcx::common::MessageEvent event)
    -> boost::asio::awaitable<void> {

  // 更新Telegram平台心跳时间
  if (db_manager_) {
    db_manager_->update_platform_heartbeat("telegram",
                                           std::chrono::system_clock::now());
  }

  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return;
  }

  // Telegram albums (media-groups) arrive as several distinct Update events
  // sharing the same `media_group_id`. If we forward each one, QQ sees them
  // as separate single-image messages. Buffer them and flush once after a
  // short debounce so the album becomes a single multi-image QQ message.
  // `is_edited_resend` events (synthesised by handle_message_edited) must
  // continue down the normal path — they reuse a single-event mapping update.
  const std::string media_group_id = get_media_group_id(event);
  const bool is_edited_resend_pre = event.data.contains("is_edited_resend") &&
                                    event.data["is_edited_resend"].get<bool>();
  if (!media_group_id.empty() && !is_edited_resend_pre) {
    auto buffer = media_group_buffer_;
    auto executor = buffer_executor_;
    // Bot pointers are stable for the plugin's lifetime (host owns them);
    // the references we have here would dangle once this coroutine returns
    // so we capture by pointer. Capture a weak_ptr to ourselves so a flush
    // that runs after the handler is dropped becomes a no-op instead of a
    // use-after-free.
    auto *tg_ptr = &telegram_bot;
    auto *qq_ptr = &qq_bot;
    std::weak_ptr<TelegramHandler> weak_self = shared_from_this();
    buffer->add(
        std::move(event), [weak_self, executor, tg_ptr, qq_ptr](
                              std::vector<obcx::common::MessageEvent> events) {
          auto self = weak_self.lock();
          if (!self) {
            return;
          }
          boost::asio::co_spawn(
              executor,
              [self, tg_ptr, qq_ptr, events = std::move(events)]() mutable
                  -> boost::asio::awaitable<void> {
                co_await self->forward_media_group_to_qq(*tg_ptr, *qq_ptr,
                                                         std::move(events));
              },
              boost::asio::detached);
        });
    co_return;
  }

  const std::string telegram_group_id = event.group_id.value();
  std::string qq_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  auto it = GROUP_MAP.find(telegram_group_id);
  if (it == GROUP_MAP.end()) {
    PLUGIN_DEBUG("tg_to_qq", "Telegram群 {} 没有对应的QQ群配置",
                 telegram_group_id);
    co_return;
  }
  bridge_config = &it->second;

  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    qq_group_id = bridge_config->qq_group_id;
    PLUGIN_DEBUG("tg_to_qq", "群组模式：Telegram群 {} 转发到QQ群 {}",
                 telegram_group_id, qq_group_id);

    if (!bridge_config->enable_tg_to_qq) {
      PLUGIN_DEBUG("tg_to_qq", "Telegram群 {} 到QQ群 {} 的转发已禁用，跳过",
                   telegram_group_id, qq_group_id);
      co_return;
    }
  } else {
    // Topic 模式：消息携带 message_thread_id 时按 topic 路由到不同 QQ 群
    int64_t message_thread_id = -1;
    if (event.data.contains("message_thread_id")) {
      message_thread_id = event.data["message_thread_id"].get<int64_t>();
    }

    const TopicBridgeConfig *topic_config =
        get_topic_config(telegram_group_id, message_thread_id);
    if (!topic_config) {
      PLUGIN_DEBUG("tg_to_qq",
                   "Telegram消息来自topic {}，没有对应的QQ群配置，跳过转发",
                   message_thread_id);
      co_return;
    }

    qq_group_id = topic_config->qq_group_id;
    PLUGIN_DEBUG("tg_to_qq", "Topic模式：Telegram topic {} 转发到QQ群 {}",
                 message_thread_id, qq_group_id);

    if (!topic_config->enable_tg_to_qq) {
      PLUGIN_DEBUG("tg_to_qq", "Telegram topic {} 到QQ群 {} 的转发已禁用，跳过",
                   message_thread_id, qq_group_id);
      co_return;
    }
  }

  if (event.raw_message.starts_with("/recall")) {
    PLUGIN_INFO("tg_to_qq", "检测到 /recall 命令，处理撤回请求");
    co_await command_handler_->handle_recall_command(telegram_bot, qq_bot,
                                                     event, qq_group_id);
    co_return;
  }

  if (event.raw_message.starts_with("/checkalive")) {
    if (GROUP_MAP.find(telegram_group_id) == GROUP_MAP.end()) {
      PLUGIN_DEBUG("tg_to_qq",
                   "Telegram群 {} 不在配置中，忽略 /checkalive 命令",
                   telegram_group_id);
      co_return;
    }

    PLUGIN_INFO("tg_to_qq", "检测到 /checkalive 命令，处理存活检查请求");
    co_await command_handler_->handle_checkalive_command(telegram_bot, qq_bot,
                                                         event, qq_group_id);
    co_return;
  }

  if (event.raw_message.starts_with("/poke")) {
    if (GROUP_MAP.find(telegram_group_id) == GROUP_MAP.end()) {
      PLUGIN_DEBUG("tg_to_qq", "Telegram群 {} 不在配置中，忽略 /poke 命令",
                   telegram_group_id);
      co_return;
    }

    PLUGIN_INFO("tg_to_qq", "检测到 /poke 命令，处理戳一戳请求");
    co_await command_handler_->handle_poke_command(telegram_bot, qq_bot, event,
                                                   qq_group_id);
    co_return;
  }

  // 忽略其他所有 / 开头的命令，不转发
  if (event.raw_message.starts_with("/")) {
    PLUGIN_DEBUG("tg_to_qq", "忽略未处理的命令消息，不转发: {}",
                 event.raw_message.substr(0, 20));
    co_return;
  }

  // 检查是否是回环消息（从QQ转发过来的）
  if (event.raw_message.starts_with("[QQ] ")) {
    PLUGIN_DEBUG("tg_to_qq", "检测到可能是回环的QQ消息，跳过转发");
    co_return;
  }

  bool is_edited_resend = event.data.contains("is_edited_resend") &&
                          event.data["is_edited_resend"].get<bool>();

  // 编辑重发时跳过去重检查，因为我们要让映射被更新
  if (!is_edited_resend &&
      db_manager_->get_target_message_id("telegram", event.message_id, "qq")
          .has_value()) {
    PLUGIN_DEBUG("tg_to_qq", "Telegram消息 {} 已转发到QQ，跳过重复处理",
                 event.message_id);
    co_return;
  }

  PLUGIN_INFO("tg_to_qq", "准备从Telegram群 {} 转发消息到QQ群 {}",
              telegram_group_id, qq_group_id);

  std::vector<std::string> temp_files_to_cleanup;
  std::vector<obcx::common::MessageSegment> message_to_send;

  try {
    db_manager_->save_user_from_event(event, "telegram");
    db_manager_->save_message_from_event(event, "telegram");

    // 处理回复消息：把被回复 TG 消息映射到对应的 QQ 消息 ID
    std::optional<std::string> reply_to_message_id;
    if (event.data.contains("reply_to_message")) {
      auto reply_to_message = event.data["reply_to_message"];
      if (reply_to_message.contains("message_id")) {
        std::string replied_message_id =
            std::to_string(reply_to_message["message_id"].get<int64_t>());

        // 情况1: 被回复的 TG 消息曾被转发到 QQ —— 引用那条 QQ 消息
        // 情况2: 被回复的 TG 消息来源于 QQ —— 引用 QQ 原始消息
        reply_to_message_id = db_manager_->get_target_message_id(
            "telegram", replied_message_id, "qq");

        if (!reply_to_message_id.has_value()) {
          reply_to_message_id = db_manager_->get_source_message_id(
              "telegram", replied_message_id, "qq");
        }

        // 找不到映射时清掉 reply_to_message，避免下游显示无效回复提示
        if (!reply_to_message_id.has_value()) {
          const_cast<nlohmann::json &>(event.data).erase("reply_to_message");
          PLUGIN_DEBUG("tg_to_qq",
                       "移除reply_to_message字段，避免显示无效回复提示");
        }

        PLUGIN_DEBUG(
            "tg_to_qq", "TG回复消息映射查找: TG消息ID {} -> QQ消息ID {}",
            replied_message_id,
            reply_to_message_id.has_value() ? reply_to_message_id.value()
                                            : "未找到");
      }
    }

    telegram::TelegramMessageFormatter::format_reply_message(
        event, reply_to_message_id, message_to_send);

    telegram::TelegramMessageFormatter::format_sender_info(
        event, bridge_config, telegram_group_id, message_to_send);

    for (const auto &segment : event.message) {
      if (segment.type != "image" && segment.type != "video" &&
          segment.type != "audio" && segment.type != "voice" &&
          segment.type != "document" && segment.type != "sticker" &&
          segment.type != "animation" && segment.type != "video_note") {
        message_to_send.push_back(segment);
      } else {
        auto media_segments = co_await media_processor_->process_media_file(
            telegram_bot, segment.type, segment.data.value("file_id", ""),
            event.data, temp_files_to_cleanup);

        for (const auto &media_segment : media_segments) {
          message_to_send.push_back(media_segment);
        }
      }
    }

    if (!message_to_send.empty()) {
      std::optional<std::string> qq_message_id;
      std::string failure_reason;

      try {
        std::string qq_response =
            co_await qq_bot.send_group_message(qq_group_id, message_to_send);

        if (!qq_response.empty()) {
          PLUGIN_DEBUG("tg_to_qq", "QQ API响应: {}", qq_response);
          nlohmann::json response_json = nlohmann::json::parse(qq_response);
          if (response_json.contains("status") &&
              response_json["status"] == "ok" &&
              response_json.contains("data") &&
              response_json["data"].is_object() &&
              response_json["data"].contains("message_id")) {
            qq_message_id = std::to_string(
                response_json["data"]["message_id"].get<int64_t>());

            if (is_edited_resend) {
              // 编辑重发：更新已有映射，不要新建
              if (!db_manager_->update_message_mapping("telegram",
                                                       event.message_id, "qq",
                                                       qq_message_id.value())) {
                PLUGIN_WARN("tg_to_qq",
                            "更新消息映射失败: telegram:{} -> qq:{}",
                            event.message_id, qq_message_id.value());
              } else {
                PLUGIN_INFO("tg_to_qq",
                            "成功更新消息映射: telegram:{} -> qq:{}",
                            event.message_id, qq_message_id.value());
              }
            } else {
              storage::MessageMapping mapping;
              mapping.source_platform = "telegram";
              mapping.source_message_id = event.message_id;
              mapping.target_platform = "qq";
              mapping.target_message_id = qq_message_id.value();
              mapping.created_at = std::chrono::system_clock::now();

              if (!db_manager_->add_message_mapping(mapping)) {
                PLUGIN_WARN("tg_to_qq",
                            "保存消息映射失败: telegram:{} -> qq:{}",
                            event.message_id, qq_message_id.value());
              }
            }

            PLUGIN_INFO("tg_to_qq", "成功转发Telegram消息到QQ: {} -> {}",
                        event.message_id, qq_message_id.value());
          } else {
            failure_reason =
                fmt::format("Invalid response format: {}", qq_response);
            PLUGIN_WARN("tg_to_qq", "QQ响应格式错误，无法提取消息ID: {}",
                        qq_response);
          }
        } else {
          failure_reason = "Empty response from QQ API";
          PLUGIN_WARN("tg_to_qq", "QQ API返回空响应");
        }
      } catch (const std::exception &e) {
        failure_reason = fmt::format("Send failed: {}", e.what());
        PLUGIN_WARN("tg_to_qq", "发送Telegram消息到QQ时出错: {}", e.what());
      }

      if (!qq_message_id.has_value() && retry_manager_ &&
          config::ENABLE_RETRY_QUEUE) {
        PLUGIN_INFO("tg_to_qq", "消息发送失败，添加到重试队列: {} -> {}",
                    event.message_id, qq_group_id);
        retry_manager_->add_message_retry(
            "telegram", "qq", event.message_id, message_to_send, qq_group_id,
            telegram_group_id, -1, config::MESSAGE_RETRY_MAX_ATTEMPTS,
            failure_reason);
      } else if (!qq_message_id.has_value()) {
        PLUGIN_ERROR("tg_to_qq", "消息发送失败且未启用重试: {}",
                     failure_reason);
      }
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理Telegram到QQ转发时出错: {}", e.what());
  }

  for (const std::string &temp_file : temp_files_to_cleanup) {
    MediaProcessor::cleanup_media_file(temp_file);
  }
}

auto TelegramHandler::handle_message_deleted(obcx::core::IBot &telegram_bot,
                                             obcx::core::IBot &qq_bot,
                                             obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_message_deleted(telegram_bot, qq_bot, event);
}

auto TelegramHandler::handle_message_edited(obcx::core::IBot &telegram_bot,
                                            obcx::core::IBot &qq_bot,
                                            obcx::common::MessageEvent event)
    -> boost::asio::awaitable<void> {
  if (db_manager_) {
    db_manager_->update_platform_heartbeat("telegram",
                                           std::chrono::system_clock::now());
  }

  co_await event_handler_->handle_message_edited(telegram_bot, qq_bot, event);
}

auto TelegramHandler::handle_recall_command(obcx::core::IBot &telegram_bot,
                                            obcx::core::IBot &qq_bot,
                                            obcx::common::MessageEvent event,
                                            std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_recall_command(telegram_bot, qq_bot, event,
                                                   qq_group_id);
}

void TelegramHandler::flush_pending_media_groups() {
  if (media_group_buffer_) {
    media_group_buffer_->flush_all_now();
  }
}

auto TelegramHandler::forward_media_group_to_qq(
    obcx::core::IBot &telegram_bot, obcx::core::IBot &qq_bot,
    std::vector<obcx::common::MessageEvent> events)
    -> boost::asio::awaitable<void> {

  if (events.empty()) {
    co_return;
  }

  // Use the first event for routing / sender / reply context. Telegram only
  // attaches caption + reply metadata to the first message of an album, so
  // this matches Telegram's own semantics.
  const obcx::common::MessageEvent &primary = events.front();

  if (db_manager_) {
    db_manager_->update_platform_heartbeat("telegram",
                                           std::chrono::system_clock::now());
  }

  if (primary.message_type != "group" || !primary.group_id.has_value()) {
    co_return;
  }

  const std::string telegram_group_id = primary.group_id.value();
  std::string qq_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  auto it = GROUP_MAP.find(telegram_group_id);
  if (it == GROUP_MAP.end()) {
    PLUGIN_DEBUG("tg_to_qq", "[media-group] Telegram群 {} 没有对应的QQ群配置",
                 telegram_group_id);
    co_return;
  }
  bridge_config = &it->second;

  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    qq_group_id = bridge_config->qq_group_id;
    if (!bridge_config->enable_tg_to_qq) {
      co_return;
    }
  } else {
    int64_t message_thread_id = -1;
    if (primary.data.contains("message_thread_id")) {
      message_thread_id = primary.data["message_thread_id"].get<int64_t>();
    }
    const TopicBridgeConfig *topic_config =
        get_topic_config(telegram_group_id, message_thread_id);
    if (!topic_config) {
      co_return;
    }
    qq_group_id = topic_config->qq_group_id;
    if (!topic_config->enable_tg_to_qq) {
      co_return;
    }
  }

  // De-dup: if the album's first message has already been forwarded (e.g. on
  // a reload race), skip. Mapping is keyed on the FIRST event's TG id only.
  if (db_manager_->get_target_message_id("telegram", primary.message_id, "qq")
          .has_value()) {
    PLUGIN_DEBUG("tg_to_qq", "media-group 主消息 {} 已转发，跳过",
                 primary.message_id);
    co_return;
  }

  PLUGIN_INFO("tg_to_qq",
              "准备转发 Telegram media-group: {} 张消息, 群 {} -> QQ群 {}",
              events.size(), telegram_group_id, qq_group_id);

  std::vector<std::string> temp_files_to_cleanup;
  std::vector<obcx::common::MessageSegment> message_to_send;

  try {
    // Persist all events for later mapping/edit lookups even though only the
    // primary one gets a cross-platform mapping row.
    for (const auto &ev : events) {
      db_manager_->save_user_from_event(ev, "telegram");
      db_manager_->save_message_from_event(ev, "telegram");
    }

    // Reply handling: only applies to the primary event. Mutate a copy so we
    // don't touch the buffered originals.
    obcx::common::MessageEvent primary_for_reply = primary;
    std::optional<std::string> reply_to_message_id;
    if (primary_for_reply.data.contains("reply_to_message")) {
      auto reply_to_message = primary_for_reply.data["reply_to_message"];
      if (reply_to_message.contains("message_id")) {
        std::string replied_message_id =
            std::to_string(reply_to_message["message_id"].get<int64_t>());
        reply_to_message_id = db_manager_->get_target_message_id(
            "telegram", replied_message_id, "qq");
        if (!reply_to_message_id.has_value()) {
          reply_to_message_id = db_manager_->get_source_message_id(
              "telegram", replied_message_id, "qq");
        }
        if (!reply_to_message_id.has_value()) {
          primary_for_reply.data.erase("reply_to_message");
        }
      }
    }

    telegram::TelegramMessageFormatter::format_reply_message(
        primary_for_reply, reply_to_message_id, message_to_send);
    telegram::TelegramMessageFormatter::format_sender_info(
        primary_for_reply, bridge_config, telegram_group_id, message_to_send);

    // Walk every event, gather media segments. Captions are stripped from the
    // per-event media_data so the dispatcher doesn't append a duplicate text
    // segment per image; we'll fold the caption(s) in once at the end.
    for (const auto &ev : events) {
      nlohmann::json media_data_no_caption = ev.data;
      media_data_no_caption.erase("caption");

      for (const auto &segment : ev.message) {
        if (segment.type != "image" && segment.type != "video" &&
            segment.type != "audio" && segment.type != "voice" &&
            segment.type != "document" && segment.type != "sticker" &&
            segment.type != "animation" && segment.type != "video_note") {
          // Non-media segments (rare for an album, but defensible) come
          // through as-is from the primary event only to avoid duplication.
          if (&ev == &events.front()) {
            message_to_send.push_back(segment);
          }
        } else {
          auto media_segments = co_await media_processor_->process_media_file(
              telegram_bot, segment.type, segment.data.value("file_id", ""),
              media_data_no_caption, temp_files_to_cleanup);
          for (const auto &media_segment : media_segments) {
            message_to_send.push_back(media_segment);
          }
        }
      }
    }

    // Combined caption: Telegram puts the caption on the first event with
    // content; concatenate any non-empty captions in arrival order so an
    // unusual album with multiple captions doesn't lose text.
    std::string combined_caption;
    for (const auto &ev : events) {
      if (ev.data.contains("caption")) {
        std::string c = ev.data.value("caption", std::string{});
        if (!c.empty()) {
          if (!combined_caption.empty()) {
            combined_caption += "\n";
          }
          combined_caption += c;
        }
      }
    }
    if (!combined_caption.empty()) {
      obcx::common::MessageSegment caption_seg;
      caption_seg.type = "text";
      caption_seg.data["text"] = combined_caption;
      message_to_send.push_back(std::move(caption_seg));
    }

    if (!message_to_send.empty()) {
      std::optional<std::string> qq_message_id;
      std::string failure_reason;

      try {
        std::string qq_response =
            co_await qq_bot.send_group_message(qq_group_id, message_to_send);
        if (!qq_response.empty()) {
          nlohmann::json response_json = nlohmann::json::parse(qq_response);
          if (response_json.contains("status") &&
              response_json["status"] == "ok" &&
              response_json.contains("data") &&
              response_json["data"].is_object() &&
              response_json["data"].contains("message_id")) {
            qq_message_id = std::to_string(
                response_json["data"]["message_id"].get<int64_t>());

            // Single mapping row keyed on the primary TG message_id only —
            // that is the id Telegram surfaces when the user replies to or
            // edits the album.
            storage::MessageMapping mapping;
            mapping.source_platform = "telegram";
            mapping.source_message_id = primary.message_id;
            mapping.target_platform = "qq";
            mapping.target_message_id = qq_message_id.value();
            mapping.created_at = std::chrono::system_clock::now();
            if (!db_manager_->add_message_mapping(mapping)) {
              PLUGIN_WARN("tg_to_qq",
                          "保存media-group消息映射失败: telegram:{} -> qq:{}",
                          primary.message_id, qq_message_id.value());
            }

            PLUGIN_INFO(
                "tg_to_qq", "成功转发Telegram media-group({}张): {} -> {}",
                events.size(), primary.message_id, qq_message_id.value());
          } else {
            failure_reason =
                fmt::format("Invalid response format: {}", qq_response);
          }
        } else {
          failure_reason = "Empty response from QQ API";
        }
      } catch (const std::exception &e) {
        failure_reason = fmt::format("Send failed: {}", e.what());
        PLUGIN_WARN("tg_to_qq", "发送 media-group 到QQ时出错: {}", e.what());
      }

      if (!qq_message_id.has_value() && retry_manager_ &&
          config::ENABLE_RETRY_QUEUE) {
        retry_manager_->add_message_retry(
            "telegram", "qq", primary.message_id, message_to_send, qq_group_id,
            telegram_group_id, -1, config::MESSAGE_RETRY_MAX_ATTEMPTS,
            failure_reason);
      } else if (!qq_message_id.has_value()) {
        PLUGIN_ERROR("tg_to_qq", "media-group 发送失败且未启用重试: {}",
                     failure_reason);
      }
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理 Telegram media-group 转发时出错: {}",
                 e.what());
  }

  for (const std::string &temp_file : temp_files_to_cleanup) {
    MediaProcessor::cleanup_media_file(temp_file);
  }
}

} // namespace bridge

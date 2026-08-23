#include "telegram/handler.hpp"
#include "bridge_state_repository.hpp"
#include "media_processor.hpp"
#include "received_message_repository.hpp"
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

auto resolve_reply_message_id(BridgeStateRepository &repository,
                              const std::string &telegram_installation,
                              const std::string &telegram_conversation,
                              const std::string &telegram_message_id,
                              const std::string &onebot_installation,
                              const std::string &qq_conversation)
    -> std::optional<std::string> {
  const auto direct = repository.resolve_target_mapping(
      {.installation_id = telegram_installation,
       .platform = "telegram",
       .conversation_id = telegram_conversation,
       .message_id = telegram_message_id},
      {.installation_id = onebot_installation,
       .platform = "qq",
       .conversation_id = qq_conversation});
  if (direct.unique()) {
    return direct.mapping->target_message_id;
  }
  if (!direct.missing()) {
    throw std::runtime_error(direct.diagnostic.empty()
                                 ? "ambiguous_message_mapping"
                                 : direct.diagnostic);
  }
  const auto reverse = repository.resolve_source_mapping(
      {.installation_id = telegram_installation,
       .platform = "telegram",
       .conversation_id = telegram_conversation,
       .message_id = telegram_message_id},
      {.installation_id = onebot_installation,
       .platform = "qq",
       .conversation_id = qq_conversation});
  if (reverse.unique()) {
    return reverse.mapping->source_message_id;
  }
  if (!reverse.missing()) {
    throw std::runtime_error(reverse.diagnostic.empty()
                                 ? "ambiguous_message_mapping"
                                 : reverse.diagnostic);
  }
  return std::nullopt;
}

} // namespace

TelegramHandler::TelegramHandler(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const BridgeConfig> config,
    std::shared_ptr<RetryQueueManager> retry_manager,
    boost::asio::any_io_executor buffer_executor,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
    std::shared_ptr<telegram::TGMediaGroupBuffer> media_group_buffer)
    : operations_(std::move(operations)), config_(std::move(config)),
      retry_manager_(std::move(retry_manager)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      blocking_executor_(std::move(blocking_executor)),
      media_processor_(std::make_unique<telegram::TelegramMediaProcessor>(
          operations_, config_, state_repository_, blocking_executor_)),
      command_handler_(std::make_unique<telegram::TelegramCommandHandler>(
          operations_, state_repository_, received_message_repository_,
          blocking_executor_)),
      event_handler_(std::make_unique<telegram::TelegramEventHandler>(
          operations_, config_, state_repository_,
          [this](obcx::common::MessageEvent event)
              -> boost::asio::awaitable<DirectForwardOutcome> {
            return forward_to_qq(std::move(event));
          },
          blocking_executor_)),
      media_group_buffer_(media_group_buffer
                              ? std::move(media_group_buffer)
                              : std::make_shared<telegram::TGMediaGroupBuffer>(
                                    buffer_executor)),
      buffer_executor_(std::move(buffer_executor)) {
  if (!operations_) {
    throw std::invalid_argument("TelegramHandler requires bot operations");
  }
}

auto TelegramHandler::forward_to_qq(obcx::common::MessageEvent event)
    -> boost::asio::awaitable<DirectForwardOutcome> {

  DirectForwardOutcome outcome;
  const auto source_installation =
      operations_->telegram_installation().installation_id;
  const auto target_installation =
      operations_->onebot11_installation().installation_id;

  // 更新Telegram平台心跳时间
  if (state_repository_) {
    const auto now = std::chrono::system_clock::now();
    (void)co_await blocking_executor_->run(
        [repository = state_repository_, source_installation, now] {
          return repository->update_platform_heartbeat(source_installation,
                                                       "telegram", now);
        });
  }

  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return outcome;
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
    std::weak_ptr<TelegramHandler> weak_self = shared_from_this();
    buffer->add(
        operations_->telegram_installation().installation_id, std::move(event),
        [weak_self, executor](std::vector<obcx::common::MessageEvent> events) {
          auto self = weak_self.lock();
          if (!self) {
            return;
          }
          boost::asio::co_spawn(
              executor,
              [self, events = std::move(
                         events)]() mutable -> boost::asio::awaitable<void> {
                co_await self->forward_media_group_to_qq(std::move(events));
              },
              boost::asio::detached);
        });
    co_return outcome;
  }

  const std::string telegram_group_id = event.group_id.value();
  std::string qq_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  bridge_config =
      config_->bridge_config(operations_->pair_id(), telegram_group_id);
  if (bridge_config == nullptr) {
    OBCX_DEBUG("Telegram群 {} 没有对应的QQ群配置", telegram_group_id);
    co_return outcome;
  }

  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    qq_group_id = bridge_config->qq_group_id;
    OBCX_DEBUG("群组模式：Telegram群 {} 转发到QQ群 {}", telegram_group_id,
               qq_group_id);

    if (!bridge_config->enable_tg_to_qq) {
      OBCX_DEBUG("Telegram群 {} 到QQ群 {} 的转发已禁用，跳过",
                 telegram_group_id, qq_group_id);
      co_return outcome;
    }
  } else {
    // Topic 模式：消息携带 message_thread_id 时按 topic 路由到不同 QQ 群
    int64_t message_thread_id = -1;
    if (event.data.contains("message_thread_id")) {
      message_thread_id = event.data["message_thread_id"].get<int64_t>();
    }

    const TopicBridgeConfig *topic_config = config_->topic_config(
        operations_->pair_id(), telegram_group_id, message_thread_id);
    if (!topic_config) {
      OBCX_DEBUG("Telegram消息来自topic {}，没有对应的QQ群配置，跳过转发",
                 message_thread_id);
      co_return outcome;
    }

    qq_group_id = topic_config->qq_group_id;
    OBCX_DEBUG("Topic模式：Telegram topic {} 转发到QQ群 {}", message_thread_id,
               qq_group_id);

    if (!topic_config->enable_tg_to_qq) {
      OBCX_DEBUG("Telegram topic {} 到QQ群 {} 的转发已禁用，跳过",
                 message_thread_id, qq_group_id);
      co_return outcome;
    }
  }

  const auto source_conversation_id =
      telegram_conversation_id(telegram_group_id);
  const auto target_conversation_id = qq_conversation_id(qq_group_id);

  // 检查是否是回环消息（从QQ转发过来的）
  if (event.raw_message.starts_with("[QQ] ")) {
    OBCX_DEBUG("检测到可能是回环的QQ消息，跳过转发");
    co_return outcome;
  }

  bool is_edited_resend = event.data.contains("is_edited_resend") &&
                          event.data["is_edited_resend"].get<bool>();

  // 编辑重发时跳过去重检查，因为我们要让映射被更新
  if (!is_edited_resend && state_repository_) {
    const auto existing = co_await blocking_executor_->run(
        [repository = state_repository_, source_installation,
         source_conversation_id, target_installation, target_conversation_id,
         message_id = event.message_id] {
          return repository->resolve_target_mapping(
              {.installation_id = source_installation,
               .platform = "telegram",
               .conversation_id = source_conversation_id,
               .message_id = message_id},
              {.installation_id = target_installation,
               .platform = "qq",
               .conversation_id = target_conversation_id},
              MessageMappingReadPurpose::PreSendDeduplication);
        });
    if (existing.unique()) {
      OBCX_DEBUG("Telegram消息 {} 已转发到QQ，跳过重复处理", event.message_id);
      co_return DirectForwardOutcome{
          .disposition = DirectForwardDisposition::AlreadyPersisted,
          .source_platform = "telegram",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "qq",
          .target_conversation_id = target_conversation_id,
          .target_message_id = existing.mapping->target_message_id,
      };
    }
    if (!existing.missing()) {
      co_return DirectForwardOutcome{
          .disposition = DirectForwardDisposition::DeliveryFailed,
          .source_platform = "telegram",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "qq",
          .target_conversation_id = target_conversation_id,
          .failure_message = existing.diagnostic.empty()
                                 ? "ambiguous_message_mapping"
                                 : existing.diagnostic,
      };
    }
  }

  OBCX_INFO("准备从Telegram群 {} 转发消息到QQ群 {}", telegram_group_id,
            qq_group_id);

  std::vector<std::string> temp_files_to_cleanup;
  std::vector<obcx::common::MessageSegment> message_to_send;

  try {
    // 处理回复消息：把被回复 TG 消息映射到对应的 QQ 消息 ID
    std::optional<std::string> reply_to_message_id;
    if (event.data.contains("reply_to_message")) {
      auto reply_to_message = event.data["reply_to_message"];
      if (reply_to_message.contains("message_id")) {
        std::string replied_message_id =
            std::to_string(reply_to_message["message_id"].get<int64_t>());

        // 情况1: 被回复的 TG 消息曾被转发到 QQ —— 引用那条 QQ 消息
        // 情况2: 被回复的 TG 消息来源于 QQ —— 引用 QQ 原始消息
        if (state_repository_) {
          reply_to_message_id = co_await blocking_executor_->run(
              [repository = state_repository_, source_installation,
               source_conversation_id, target_installation,
               target_conversation_id, replied_message_id] {
                return resolve_reply_message_id(
                    *repository, source_installation, source_conversation_id,
                    replied_message_id, target_installation,
                    target_conversation_id);
              });
        }

        // 找不到映射时清掉 reply_to_message，避免下游显示无效回复提示
        if (!reply_to_message_id.has_value()) {
          const_cast<nlohmann::json &>(event.data).erase("reply_to_message");
          OBCX_DEBUG("移除reply_to_message字段，避免显示无效回复提示");
        }

        OBCX_DEBUG("TG回复消息映射查找: TG消息ID {} -> QQ消息ID {}",
                   replied_message_id,
                   reply_to_message_id.has_value() ? reply_to_message_id.value()
                                                   : "未找到");
      }
    }

    telegram::TelegramMessageFormatter::format_reply_message(
        event, reply_to_message_id, message_to_send);

    telegram::TelegramMessageFormatter::format_sender_info(
        *config_, event, bridge_config, telegram_group_id, message_to_send,
        operations_->pair_id());

    for (const auto &segment : event.message) {
      if (segment.type != "image" && segment.type != "video" &&
          segment.type != "audio" && segment.type != "voice" &&
          segment.type != "document" && segment.type != "sticker" &&
          segment.type != "animation" && segment.type != "video_note") {
        message_to_send.push_back(segment);
      } else {
        auto media_segments = co_await media_processor_->process_media_file(
            segment.type, segment.data.value("file_id", ""), event.data,
            temp_files_to_cleanup);

        for (const auto &media_segment : media_segments) {
          message_to_send.push_back(media_segment);
        }
      }
    }

    if (!message_to_send.empty()) {
      std::optional<std::string> qq_message_id;
      std::string failure_reason;
      bool definitely_retryable = false;
      bool outcome_unknown = false;

      try {
        qq_message_id = co_await operations_->send_onebot11_group(
            qq_group_id, message_to_send);
        outcome = DirectForwardOutcome{
            .disposition = DirectForwardDisposition::NewDelivery,
            .source_platform = "telegram",
            .source_conversation_id = source_conversation_id,
            .source_message_id = event.message_id,
            .target_platform = "qq",
            .target_conversation_id = target_conversation_id,
            .target_message_id = *qq_message_id,
        };
      } catch (const std::exception &error) {
        const auto disposition = classify_bridge_operation_failure(error);
        failure_reason = disposition.diagnostic;
        definitely_retryable = disposition.retryable;
        outcome_unknown = disposition.outcome_unknown;
        outcome = DirectForwardOutcome{
            .disposition = DirectForwardDisposition::DeliveryFailed,
            .source_platform = "telegram",
            .source_conversation_id = source_conversation_id,
            .source_message_id = event.message_id,
            .target_platform = "qq",
            .target_conversation_id = target_conversation_id,
            .failure_message = failure_reason,
            .failure_retryable = definitely_retryable,
        };
        OBCX_WARN("发送Telegram消息到QQ时出错: {}", failure_reason);
      }

      if (!qq_message_id.has_value() && definitely_retryable &&
          retry_manager_ && config_->enable_retry_queue) {
        OBCX_INFO("消息发送失败，添加到重试队列: {} -> {}", event.message_id,
                  qq_group_id);
        co_await blocking_executor_->run(
            [retry_manager = retry_manager_, source_installation,
             source_conversation_id, target_installation,
             target_conversation_id, message_id = event.message_id,
             message = message_to_send, qq_group_id, telegram_group_id,
             max_attempts = config_->message_retry_max_attempts,
             failure_reason] {
              retry_manager->add_message_retry(
                  source_installation, "telegram", source_conversation_id,
                  target_installation, "qq", target_conversation_id, message_id,
                  message, qq_group_id, telegram_group_id, -1, max_attempts,
                  failure_reason);
            });
      } else if (!qq_message_id.has_value() && outcome_unknown) {
        OBCX_ERROR("消息发送结果不确定，禁止自动重试: {}", failure_reason);
      } else if (!qq_message_id.has_value() && definitely_retryable &&
                 config_->enable_retry_queue) {
        OBCX_ERROR("消息发送失败且重试队列不可用: {}", failure_reason);
      } else if (!qq_message_id.has_value()) {
        OBCX_ERROR("消息发送失败且未启用重试: {}", failure_reason);
      }
    } else if (!event.message.empty()) {
      outcome = DirectForwardOutcome{
          .disposition = DirectForwardDisposition::DeliveryFailed,
          .source_platform = "telegram",
          .source_conversation_id = source_conversation_id,
          .source_message_id = event.message_id,
          .target_platform = "qq",
          .target_conversation_id = target_conversation_id,
          .failure_message = "bridge_media_processing_failed",
      };
    }

  } catch (const std::exception &e) {
    OBCX_ERROR("处理Telegram到QQ转发时出错: {}", e.what());
    const auto mapping_error = std::string_view{e.what()}.find(
                                   "message_mapping") != std::string_view::npos;
    outcome = DirectForwardOutcome{
        .disposition = DirectForwardDisposition::DeliveryFailed,
        .source_platform = "telegram",
        .source_conversation_id = source_conversation_id,
        .source_message_id = event.message_id,
        .target_platform = "qq",
        .target_conversation_id = target_conversation_id,
        .failure_message =
            mapping_error ? e.what() : "bridge_processing_failed",
    };
  }

  if (!temp_files_to_cleanup.empty()) {
    co_await blocking_executor_->run(
        [files = std::move(temp_files_to_cleanup)] {
          for (const auto &file : files) {
            MediaProcessor::cleanup_media_file(file);
          }
        });
  }
  co_return outcome;
}

auto TelegramHandler::handle_message_deleted(obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_message_deleted(std::move(event));
}

auto TelegramHandler::handle_message_edited(obcx::common::MessageEvent event)
    -> boost::asio::awaitable<DirectForwardOutcome> {
  if (state_repository_) {
    const auto now = std::chrono::system_clock::now();
    const auto installation =
        operations_->telegram_installation().installation_id;
    (void)co_await blocking_executor_->run(
        [repository = state_repository_, installation, now] {
          return repository->update_platform_heartbeat(installation, "telegram",
                                                       now);
        });
  }

  co_return co_await event_handler_->handle_message_edited(std::move(event));
}

auto TelegramHandler::handle_recall_command(obcx::common::MessageEvent event,
                                            std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_recall_command(std::move(event),
                                                   qq_group_id);
}

auto TelegramHandler::handle_checkalive_command(
    obcx::common::MessageEvent event, std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_checkalive_command(std::move(event),
                                                       qq_group_id);
}

auto TelegramHandler::handle_poke_command(obcx::common::MessageEvent event,
                                          std::string_view qq_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_poke_command(std::move(event), qq_group_id);
}

void TelegramHandler::flush_pending_media_groups() {
  if (media_group_buffer_) {
    media_group_buffer_->flush_all_now();
  }
}

auto TelegramHandler::forward_media_group_to_qq(
    std::vector<obcx::common::MessageEvent> events)
    -> boost::asio::awaitable<void> {

  if (events.empty()) {
    co_return;
  }

  // Use the first event for routing / sender / reply context. Telegram only
  // attaches caption + reply metadata to the first message of an album, so
  // this matches Telegram's own semantics.
  const obcx::common::MessageEvent &primary = events.front();
  const auto source_installation =
      operations_->telegram_installation().installation_id;
  const auto target_installation =
      operations_->onebot11_installation().installation_id;

  if (state_repository_) {
    const auto now = std::chrono::system_clock::now();
    (void)co_await blocking_executor_->run(
        [repository = state_repository_, source_installation, now] {
          return repository->update_platform_heartbeat(source_installation,
                                                       "telegram", now);
        });
  }

  if (primary.message_type != "group" || !primary.group_id.has_value()) {
    co_return;
  }

  const std::string telegram_group_id = primary.group_id.value();
  std::string qq_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  bridge_config =
      config_->bridge_config(operations_->pair_id(), telegram_group_id);
  if (bridge_config == nullptr) {
    OBCX_DEBUG("[media-group] Telegram群 {} 没有对应的QQ群配置",
               telegram_group_id);
    co_return;
  }

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
    const TopicBridgeConfig *topic_config = config_->topic_config(
        operations_->pair_id(), telegram_group_id, message_thread_id);
    if (!topic_config) {
      co_return;
    }
    qq_group_id = topic_config->qq_group_id;
    if (!topic_config->enable_tg_to_qq) {
      co_return;
    }
  }

  const auto source_conversation_id =
      telegram_conversation_id(telegram_group_id);
  const auto target_conversation_id = qq_conversation_id(qq_group_id);

  // De-dup: if any album item has already been forwarded (e.g. on a reload
  // race), skip sending again. While skipping, repair any missing rows so all
  // TG ids in the album resolve to the same QQ message id.
  MessageMappingResolution existing;
  if (state_repository_) {
    existing = co_await blocking_executor_->run(
        [repository = state_repository_, events, qq_group_id,
         source_installation, source_conversation_id, target_installation,
         target_conversation_id] {
          MessageMappingResolution found;
          for (const auto &event : events) {
            found = repository->resolve_target_mapping(
                {.installation_id = source_installation,
                 .platform = "telegram",
                 .conversation_id = source_conversation_id,
                 .message_id = event.message_id},
                {.installation_id = target_installation,
                 .platform = "qq",
                 .conversation_id = target_conversation_id});
            if (!found.missing()) {
              break;
            }
          }
          if (!found.unique()) {
            return found;
          }
          for (std::size_t index = 0; index < events.size(); ++index) {
            const auto &event = events[index];
            storage::MessageMapping mapping{
                .source_installation = source_installation,
                .source_platform = "telegram",
                .source_conversation_id = source_conversation_id,
                .source_message_id = event.message_id,
                .target_installation = target_installation,
                .target_platform = "qq",
                .target_conversation_id = target_conversation_id,
                .target_message_id = found.mapping->target_message_id,
                .is_primary = index == 0,
                .created_at = std::chrono::system_clock::now(),
            };
            if (!repository->add_message_mapping(
                    mapping, MessageMappingWritePurpose::DeferredMediaGroup)) {
              OBCX_WARN("修复media-group消息映射失败: telegram:{} -> qq:{}",
                        event.message_id, found.mapping->target_message_id);
            }
            const auto media_group_id = get_media_group_id(event);
            if (!media_group_id.empty()) {
              (void)repository->add_media_group_mapping(MediaGroupMapping{
                  .source_installation = source_installation,
                  .source_platform = "telegram",
                  .source_conversation_id = source_conversation_id,
                  .media_group_id = media_group_id,
                  .source_message_id = event.message_id,
                  .target_installation = target_installation,
                  .target_platform = "qq",
                  .target_conversation_id = target_conversation_id,
                  .target_message_id = found.mapping->target_message_id,
                  .target_group_id = qq_group_id,
                  .is_primary = index == 0,
                  .created_at = std::chrono::system_clock::now()});
            }
          }
          return found;
        });
  }
  if (existing.unique()) {
    OBCX_DEBUG("media-group 已转发到QQ消息 {}，跳过重复发送",
               existing.mapping->target_message_id);
    co_return;
  }
  if (!existing.missing()) {
    OBCX_ERROR("media-group mapping resolution failed: {}",
               existing.diagnostic.empty() ? "ambiguous_message_mapping"
                                           : existing.diagnostic);
    co_return;
  }

  OBCX_INFO("准备转发 Telegram media-group: {} 张消息, 群 {} -> QQ群 {}",
            events.size(), telegram_group_id, qq_group_id);

  std::vector<std::string> temp_files_to_cleanup;
  std::vector<obcx::common::MessageSegment> message_to_send;

  try {
    // Reply handling: only applies to the primary event. Mutate a copy so we
    // don't touch the buffered originals.
    obcx::common::MessageEvent primary_for_reply = primary;
    std::optional<std::string> reply_to_message_id;
    if (primary_for_reply.data.contains("reply_to_message")) {
      auto reply_to_message = primary_for_reply.data["reply_to_message"];
      if (reply_to_message.contains("message_id")) {
        std::string replied_message_id =
            std::to_string(reply_to_message["message_id"].get<int64_t>());
        if (state_repository_) {
          reply_to_message_id = co_await blocking_executor_->run(
              [repository = state_repository_, source_installation,
               source_conversation_id, target_installation,
               target_conversation_id, replied_message_id] {
                return resolve_reply_message_id(
                    *repository, source_installation, source_conversation_id,
                    replied_message_id, target_installation,
                    target_conversation_id);
              });
        }
        if (!reply_to_message_id.has_value()) {
          primary_for_reply.data.erase("reply_to_message");
        }
      }
    }

    telegram::TelegramMessageFormatter::format_reply_message(
        primary_for_reply, reply_to_message_id, message_to_send);
    telegram::TelegramMessageFormatter::format_sender_info(
        *config_, primary_for_reply, bridge_config, telegram_group_id,
        message_to_send, operations_->pair_id());

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
              segment.type, segment.data.value("file_id", ""),
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
      bool definitely_retryable = false;
      bool outcome_unknown = false;

      try {
        qq_message_id = co_await operations_->send_onebot11_group(
            qq_group_id, message_to_send);
        if (state_repository_) {
          co_await blocking_executor_->run(
              [repository = state_repository_, events,
               target_message_id = *qq_message_id, qq_group_id,
               source_installation, source_conversation_id, target_installation,
               target_conversation_id] {
                for (std::size_t index = 0; index < events.size(); ++index) {
                  const auto &event = events[index];
                  storage::MessageMapping mapping{
                      .source_installation = source_installation,
                      .source_platform = "telegram",
                      .source_conversation_id = source_conversation_id,
                      .source_message_id = event.message_id,
                      .target_installation = target_installation,
                      .target_platform = "qq",
                      .target_conversation_id = target_conversation_id,
                      .target_message_id = target_message_id,
                      .is_primary = index == 0,
                      .created_at = std::chrono::system_clock::now(),
                  };
                  if (!repository->add_message_mapping(
                          mapping,
                          MessageMappingWritePurpose::DeferredMediaGroup)) {
                    OBCX_WARN(
                        "保存media-group消息映射失败: telegram:{} -> qq:{}",
                        event.message_id, target_message_id);
                  }
                  const auto media_group_id = get_media_group_id(event);
                  if (!media_group_id.empty()) {
                    (void)repository->add_media_group_mapping(MediaGroupMapping{
                        .source_installation = source_installation,
                        .source_platform = "telegram",
                        .source_conversation_id = source_conversation_id,
                        .media_group_id = media_group_id,
                        .source_message_id = event.message_id,
                        .target_installation = target_installation,
                        .target_platform = "qq",
                        .target_conversation_id = target_conversation_id,
                        .target_message_id = target_message_id,
                        .target_group_id = qq_group_id,
                        .is_primary = index == 0,
                        .created_at = std::chrono::system_clock::now()});
                  }
                }
              });
        }
      } catch (const std::exception &error) {
        const auto disposition = classify_bridge_operation_failure(error);
        failure_reason = disposition.diagnostic;
        definitely_retryable = disposition.retryable;
        outcome_unknown = disposition.outcome_unknown;
        OBCX_WARN("发送 media-group 到QQ时出错: {}", failure_reason);
      }

      if (!qq_message_id.has_value() && definitely_retryable &&
          retry_manager_ && config_->enable_retry_queue) {
        co_await blocking_executor_->run(
            [retry_manager = retry_manager_, source_installation,
             source_conversation_id, target_installation,
             target_conversation_id, message_id = primary.message_id,
             message = message_to_send, qq_group_id, telegram_group_id,
             max_attempts = config_->message_retry_max_attempts,
             failure_reason] {
              retry_manager->add_message_retry(
                  source_installation, "telegram", source_conversation_id,
                  target_installation, "qq", target_conversation_id, message_id,
                  message, qq_group_id, telegram_group_id, -1, max_attempts,
                  failure_reason);
            });
      } else if (!qq_message_id.has_value() && outcome_unknown) {
        OBCX_ERROR("media-group 发送结果不确定，禁止自动重试: {}",
                   failure_reason);
      } else if (!qq_message_id.has_value()) {
        OBCX_ERROR("media-group 发送失败且未启用重试: {}", failure_reason);
      }
    }
  } catch (const std::exception &e) {
    OBCX_ERROR("处理 Telegram media-group 转发时出错: {}", e.what());
  }

  if (!temp_files_to_cleanup.empty()) {
    co_await blocking_executor_->run(
        [files = std::move(temp_files_to_cleanup)] {
          for (const auto &file : files) {
            MediaProcessor::cleanup_media_file(file);
          }
        });
  }
}

} // namespace bridge

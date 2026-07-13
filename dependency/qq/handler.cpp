#include "qq/handler.hpp"
#include "bridge_state_repository.hpp"
#include "config.hpp"
#include "media_processor.hpp"
#include "qq/command_handler.hpp"
#include "qq/event_handler.hpp"
#include "qq/media_processor.hpp"
#include "qq/message_formatter.hpp"
#include "received_message_repository.hpp"
#include "retry_queue_manager.hpp"

#include <common/logger.hpp>
#include <interfaces/telegram_bot.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge {

QQHandler::QQHandler(
    std::shared_ptr<RetryQueueManager> retry_manager,
    std::shared_ptr<BridgeStateRepository> state_repository,
    std::shared_ptr<ReceivedMessageRepository> received_message_repository)
    : retry_manager_(std::move(retry_manager)),
      state_repository_(std::move(state_repository)),
      received_message_repository_(std::move(received_message_repository)),
      media_processor_(std::make_unique<qq::QQMediaProcessor>(state_repository_)),
      command_handler_(std::make_unique<qq::QQCommandHandler>(state_repository_)),
      event_handler_(std::make_unique<qq::QQEventHandler>(
          state_repository_, received_message_repository_)),
      message_formatter_(std::make_unique<qq::QQMessageFormatter>(
          state_repository_)) {}

// 析构函数需要在这里定义，以确保所有子模块类的完整定义都可见
QQHandler::~QQHandler() = default;

auto QQHandler::forward_to_telegram(obcx::core::IBot &telegram_bot,
                                    obcx::core::IBot &qq_bot,
                                    obcx::common::MessageEvent event)
    -> boost::asio::awaitable<void> {
  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return;
  }

  const std::string qq_group_id = event.group_id.value();
  std::string telegram_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  auto [tg_id, topic_id] = get_tg_group_and_topic_id(qq_group_id);
  OBCX_DEBUG("QQ群 {} 查找结果: TG群={}, topic_id={}",
               qq_group_id, tg_id, topic_id);

  if (tg_id.empty()) {
    OBCX_DEBUG("QQ群 {} 没有对应的Telegram群配置", qq_group_id);
    co_return;
  }

  telegram_group_id = tg_id;
  bridge_config = get_bridge_config(telegram_group_id);

  if (!bridge_config) {
    OBCX_DEBUG("无法找到Telegram群 {} 的配置", telegram_group_id);
    co_return;
  }

  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    if (!bridge_config->enable_qq_to_tg) {
      OBCX_DEBUG("QQ群 {} 到Telegram群 {} 的转发已禁用，跳过",
                   qq_group_id, telegram_group_id);
      co_return;
    }
  } else if (bridge_config->mode == BridgeMode::TOPIC_TO_GROUP) {
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    if (!topic_config || !topic_config->enable_qq_to_tg) {
      OBCX_DEBUG("QQ群 {} 到Telegram topic {} 的转发已禁用，跳过",
                   qq_group_id, topic_id);
      co_return;
    }
  }

  // /checkalive 是唯一会被转发逻辑短路成回应的命令；其它 /
  // 开头的消息一律不转发。
  if (event.raw_message.starts_with("/checkalive")) {
    OBCX_INFO("检测到 /checkalive 命令，处理存活检查请求");
    co_await command_handler_->handle_checkalive_command(
        telegram_bot, qq_bot, event, telegram_group_id);
    co_return;
  }

  if (event.raw_message.starts_with("/")) {
    OBCX_DEBUG("忽略未处理的命令消息，不转发: {}",
                 event.raw_message.substr(0, 20));
    co_return;
  }

  // 跳过从 Telegram 转发回来的回环消息
  if (event.raw_message.starts_with("[Telegram] ")) {
    OBCX_DEBUG("检测到可能是回环的Telegram消息，跳过转发");
    co_return;
  }

  const auto existing_target_message_id =
      state_repository_
          ? state_repository_->get_target_message_id("qq", event.message_id,
                                                     "telegram")
          : std::optional<std::string>{};
  if (existing_target_message_id.has_value()) {
    OBCX_DEBUG("QQ消息 {} 已转发到Telegram，跳过重复处理",
                 event.message_id);
    co_return;
  }

  OBCX_INFO("准备从QQ群 {} 转发消息到Telegram群 {}", qq_group_id,
              telegram_group_id);

  try {
    obcx::common::Message message_to_send;

    std::string sender_display_name =
        co_await message_formatter_->format_sender_info(
            qq_bot, event, bridge_config, qq_group_id, telegram_group_id,
            topic_id, message_to_send);

    co_await message_formatter_->format_reply_message(event, message_to_send);

    std::vector<obcx::common::MessageSegment> image_segments;
    std::vector<obcx::common::MessageSegment> other_segments;

    for (const auto &segment : event.message) {
      if (segment.type == "reply") {
        continue; // reply 段已经在 format_reply_message 里消费过了
      }

      if (segment.type == "image") {
        image_segments.push_back(segment);
      } else {
        other_segments.push_back(segment);
      }
    }

    // 多张图片走 sendMediaGroup；单张或失败时退回到逐段发送（下方循环）。
    bool media_group_sent = co_await message_formatter_->send_media_group(
        telegram_bot, image_segments, other_segments, telegram_group_id,
        topic_id, sender_display_name, bridge_config, message_to_send, event);

    if (media_group_sent) {
      co_return;
    }

    std::vector<std::string> temp_files_to_cleanup;

    for (const auto &img_segment : image_segments) {
      auto converted_segment =
          co_await media_processor_->process_qq_media_segment(
              qq_bot, telegram_bot, img_segment, event, telegram_group_id,
              topic_id, sender_display_name, bridge_config,
              temp_files_to_cleanup);

      if (converted_segment.has_value()) {
        message_to_send.push_back(converted_segment.value());
      }
    }

    for (const auto &segment : other_segments) {
      if (segment.type == "forward") {
        co_await message_formatter_->process_forward_message(
            qq_bot, telegram_bot, segment, telegram_group_id, topic_id,
            message_to_send);
        continue;
      }

      if (segment.type == "node") {
        co_await message_formatter_->process_node_message(segment,
                                                          message_to_send);
        continue;
      }

      auto converted_segment =
          co_await media_processor_->process_qq_media_segment(
              qq_bot, telegram_bot, segment, event, telegram_group_id, topic_id,
              sender_display_name, bridge_config, temp_files_to_cleanup);

      if (converted_segment.has_value()) {
        message_to_send.push_back(converted_segment.value());
      }
    }

    std::optional<std::string> telegram_message_id;
    std::string failure_reason;

    try {
      std::string response;
      if (topic_id == -1) {
        response = co_await telegram_bot.send_group_message(telegram_group_id,
                                                            message_to_send);
        OBCX_DEBUG("群组模式：QQ群 {} 转发到Telegram群 {}",
                     qq_group_id, telegram_group_id);
      } else {
        auto &tg_bot =
            dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);
        response = co_await tg_bot.send_topic_message(
            telegram_group_id, topic_id, message_to_send);
        OBCX_DEBUG("Topic模式：QQ群 {} 转发到Telegram群 {} 的topic {}",
                     qq_group_id, telegram_group_id, topic_id);
      }

      if (!response.empty()) {
        OBCX_DEBUG("Telegram API响应: {}", response);
        nlohmann::json response_json = nlohmann::json::parse(response);
        if (response_json.contains("result") &&
            response_json["result"].is_object() &&
            response_json["result"].contains("message_id")) {
          telegram_message_id = std::to_string(
              response_json["result"]["message_id"].get<int64_t>());

          storage::MessageMapping mapping;
          mapping.source_platform = "qq";
          mapping.source_message_id = event.message_id;
          mapping.target_platform = "telegram";
          mapping.target_message_id = telegram_message_id.value();
          mapping.created_at = std::chrono::system_clock::now();
          if (state_repository_) {
            state_repository_->add_message_mapping(mapping);
          }

          OBCX_INFO("QQ消息 {} 成功转发到Telegram，Telegram消息ID: {}",
                      event.message_id, telegram_message_id.value());
        } else {
          failure_reason = fmt::format("Invalid response format: {}", response);
          OBCX_WARN("转发QQ消息后，无法解析Telegram消息ID。响应: {}",
                      response);
        }
      } else {
        failure_reason = "Empty response from Telegram API";
        OBCX_WARN("Telegram API返回空响应");
      }
    } catch (const std::exception &e) {
      failure_reason = fmt::format("Send failed: {}", e.what());
      OBCX_WARN("发送QQ消息到Telegram时出错: {}", e.what());
    }

    if (!telegram_message_id.has_value() && retry_manager_ &&
        config::ENABLE_RETRY_QUEUE) {
      OBCX_INFO("消息发送失败，添加到重试队列: {} -> {}",
                  event.message_id, telegram_group_id);
      retry_manager_->add_message_retry(
          "qq", "telegram", event.message_id, message_to_send,
          telegram_group_id, qq_group_id, topic_id,
          config::MESSAGE_RETRY_MAX_ATTEMPTS, failure_reason);
    } else if (!telegram_message_id.has_value()) {
      OBCX_ERROR("消息发送失败且未启用重试: {}", failure_reason);
    }

    for (const std::string &temp_file : temp_files_to_cleanup) {
      MediaProcessor::cleanup_media_file(temp_file);
    }

  } catch (const std::exception &e) {
    OBCX_ERROR("转发QQ消息到Telegram时出错: {}", e.what());
    qq_bot.error_notify(
        qq_group_id, fmt::format("转发消息到Telegram失败: {}", e.what()), true);
  }
}

auto QQHandler::handle_recall_event(obcx::core::IBot &telegram_bot,
                                    obcx::core::IBot &qq_bot,
                                    obcx::common::Event event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_recall_event(telegram_bot, qq_bot, event);
}

auto QQHandler::handle_checkalive_command(obcx::core::IBot &telegram_bot,
                                          obcx::core::IBot &qq_bot,
                                          obcx::common::MessageEvent event,
                                          const std::string &telegram_group_id)
    -> boost::asio::awaitable<void> {
  co_await command_handler_->handle_checkalive_command(
      telegram_bot, qq_bot, event, telegram_group_id);
}

auto QQHandler::handle_poke_event(obcx::core::IBot &telegram_bot,
                                  obcx::core::IBot &qq_bot,
                                  const obcx::common::NoticeEvent &event)
    -> boost::asio::awaitable<void> {
  co_await event_handler_->handle_poke_event(telegram_bot, qq_bot, event);
}

} // namespace bridge

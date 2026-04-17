#include "qq/handler.hpp"
#include "config.hpp"
#include "media_processor.hpp"
#include "qq/command_handler.hpp"
#include "qq/event_handler.hpp"
#include "qq/media_processor.hpp"
#include "qq/message_formatter.hpp"
#include "retry_queue_manager.hpp"

#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge {

QQHandler::QQHandler(std::shared_ptr<storage::DatabaseManager> db_manager,
                     std::shared_ptr<RetryQueueManager> retry_manager)
    : db_manager_(std::move(db_manager)),
      retry_manager_(std::move(retry_manager)),
      media_processor_(std::make_unique<qq::QQMediaProcessor>(db_manager_)),
      command_handler_(std::make_unique<qq::QQCommandHandler>(db_manager_)),
      event_handler_(std::make_unique<qq::QQEventHandler>(db_manager_)),
      message_formatter_(
          std::make_unique<qq::QQMessageFormatter>(db_manager_)) {}

// 析构函数需要在这里定义，以确保所有子模块类的完整定义都可见
QQHandler::~QQHandler() = default;

auto QQHandler::forward_to_telegram(obcx::core::IBot &telegram_bot,
                                    obcx::core::IBot &qq_bot,
                                    obcx::common::MessageEvent event)
    -> boost::asio::awaitable<void> {
  // 确保是群消息
  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return;
  }

  const std::string qq_group_id = event.group_id.value();
  std::string telegram_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  // 查找对应的Telegram群ID、topic ID和桥接配置
  auto [tg_id, topic_id] = get_tg_group_and_topic_id(qq_group_id);
  PLUGIN_DEBUG("qq_to_tg", "QQ群 {} 查找结果: TG群={}, topic_id={}",
               qq_group_id, tg_id, topic_id);

  if (tg_id.empty()) {
    PLUGIN_DEBUG("qq_to_tg", "QQ群 {} 没有对应的Telegram群配置", qq_group_id);
    co_return;
  }

  telegram_group_id = tg_id;
  bridge_config = get_bridge_config(telegram_group_id);

  if (!bridge_config) {
    PLUGIN_DEBUG("qq_to_tg", "无法找到Telegram群 {} 的配置", telegram_group_id);
    co_return;
  }

  // 检查是否启用QQ到TG转发
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    if (!bridge_config->enable_qq_to_tg) {
      PLUGIN_DEBUG("qq_to_tg", "QQ群 {} 到Telegram群 {} 的转发已禁用，跳过",
                   qq_group_id, telegram_group_id);
      co_return;
    }
  } else if (bridge_config->mode == BridgeMode::TOPIC_TO_GROUP) {
    // Topic模式：需要检查具体的topic配置
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    if (!topic_config || !topic_config->enable_qq_to_tg) {
      PLUGIN_DEBUG("qq_to_tg", "QQ群 {} 到Telegram topic {} 的转发已禁用，跳过",
                   qq_group_id, topic_id);
      co_return;
    }
  }

  // 检查是否是 /checkalive 命令
  if (event.raw_message.starts_with("/checkalive")) {
    PLUGIN_INFO("qq_to_tg", "检测到 /checkalive 命令，处理存活检查请求");
    co_await command_handler_->handle_checkalive_command(
        telegram_bot, qq_bot, event, telegram_group_id);
    co_return;
  }

  // 忽略其他所有 / 开头的命令，不转发
  if (event.raw_message.starts_with("/")) {
    PLUGIN_DEBUG("qq_to_tg", "忽略未处理的命令消息，不转发: {}",
                 event.raw_message.substr(0, 20));
    co_return;
  }

  // 检查是否是回环消息（从Telegram转发过来的）
  if (event.raw_message.starts_with("[Telegram] ")) {
    PLUGIN_DEBUG("qq_to_tg", "检测到可能是回环的Telegram消息，跳过转发");
    co_return;
  }

  // 检查消息是否已转发（避免重复）
  if (db_manager_->get_target_message_id("qq", event.message_id, "telegram")
          .has_value()) {
    PLUGIN_DEBUG("qq_to_tg", "QQ消息 {} 已转发到Telegram，跳过重复处理",
                 event.message_id);
    co_return;
  }

  PLUGIN_INFO("qq_to_tg", "准备从QQ群 {} 转发消息到Telegram群 {}", qq_group_id,
              telegram_group_id);

  try {
    // 保存/更新用户信息
    db_manager_->save_user_from_event(event, "qq");
    // 保存消息信息
    db_manager_->save_message_from_event(event, "qq");

    // 创建转发消息，保留原始消息的所有段（包括图片）
    obcx::common::Message message_to_send;

    // 格式化发送者信息并获取显示名称
    std::string sender_display_name =
        co_await message_formatter_->format_sender_info(
            qq_bot, event, bridge_config, qq_group_id, telegram_group_id,
            topic_id, message_to_send);

    // 处理回复消息
    co_await message_formatter_->format_reply_message(event, message_to_send);

    // 先收集消息中的所有图片，用于批量处理
    std::vector<obcx::common::MessageSegment> image_segments;
    std::vector<obcx::common::MessageSegment> other_segments;

    for (const auto &segment : event.message) {
      if (segment.type == "reply") {
        continue; // 跳过reply段，已经处理过了
      }

      if (segment.type == "image") {
        image_segments.push_back(segment);
      } else {
        other_segments.push_back(segment);
      }
    }

    // 尝试批量处理图片（如果有多张图片）
    bool media_group_sent = co_await message_formatter_->send_media_group(
        telegram_bot, image_segments, other_segments, telegram_group_id,
        topic_id, sender_display_name, bridge_config, message_to_send, event);

    if (media_group_sent) {
      co_return; // MediaGroup发送成功，直接返回
    }

    // 用于收集下载的临时文件路径，以便发送后清理
    std::vector<std::string> temp_files_to_cleanup;

    // 处理单张图片或MediaGroup发送失败时的回退处理
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

    // 处理其他类型的消息段
    for (const auto &segment : other_segments) {
      // 特殊处理合并转发消息
      if (segment.type == "forward") {
        co_await message_formatter_->process_forward_message(
            qq_bot, telegram_bot, segment, telegram_group_id, topic_id,
            message_to_send);
        continue;
      }

      // 处理单个node消息段（自定义转发节点）
      if (segment.type == "node") {
        co_await message_formatter_->process_node_message(segment,
                                                          message_to_send);
        continue;
      }

      // 处理其他消息类型
      auto converted_segment =
          co_await media_processor_->process_qq_media_segment(
              qq_bot, telegram_bot, segment, event, telegram_group_id, topic_id,
              sender_display_name, bridge_config, temp_files_to_cleanup);

      if (converted_segment.has_value()) {
        message_to_send.push_back(converted_segment.value());
      }
    }

    // 发送到Telegram群或特定topic（支持重试）
    std::optional<std::string> telegram_message_id;
    std::string failure_reason;

    try {
      std::string response;
      if (topic_id == -1) {
        // 群组模式：发送到群组
        response = co_await telegram_bot.send_group_message(telegram_group_id,
                                                            message_to_send);
        PLUGIN_DEBUG("qq_to_tg", "群组模式：QQ群 {} 转发到Telegram群 {}",
                     qq_group_id, telegram_group_id);
      } else {
        // Topic模式：发送到特定topic
        auto &tg_bot = static_cast<obcx::core::TGBot &>(telegram_bot);
        response = co_await tg_bot.send_topic_message(
            telegram_group_id, topic_id, message_to_send);
        PLUGIN_DEBUG("qq_to_tg",
                     "Topic模式：QQ群 {} 转发到Telegram群 {} 的topic {}",
                     qq_group_id, telegram_group_id, topic_id);
      }

      // 解析响应获取Telegram消息ID
      if (!response.empty()) {
        PLUGIN_DEBUG("qq_to_tg", "Telegram API响应: {}", response);
        nlohmann::json response_json = nlohmann::json::parse(response);
        if (response_json.contains("result") &&
            response_json["result"].is_object() &&
            response_json["result"].contains("message_id")) {
          telegram_message_id = std::to_string(
              response_json["result"]["message_id"].get<int64_t>());

          // 记录消息ID映射
          storage::MessageMapping mapping;
          mapping.source_platform = "qq";
          mapping.source_message_id = event.message_id;
          mapping.target_platform = "telegram";
          mapping.target_message_id = telegram_message_id.value();
          mapping.created_at = std::chrono::system_clock::now();
          db_manager_->add_message_mapping(mapping);

          PLUGIN_INFO("qq_to_tg",
                      "QQ消息 {} 成功转发到Telegram，Telegram消息ID: {}",
                      event.message_id, telegram_message_id.value());
        } else {
          failure_reason = fmt::format("Invalid response format: {}", response);
          PLUGIN_WARN("qq_to_tg",
                      "转发QQ消息后，无法解析Telegram消息ID。响应: {}",
                      response);
        }
      } else {
        failure_reason = "Empty response from Telegram API";
        PLUGIN_WARN("qq_to_tg", "Telegram API返回空响应");
      }
    } catch (const std::exception &e) {
      failure_reason = fmt::format("Send failed: {}", e.what());
      PLUGIN_WARN("qq_to_tg", "发送QQ消息到Telegram时出错: {}", e.what());
    }

    // 如果发送失败且启用了重试队列，添加到重试队列
    if (!telegram_message_id.has_value() && retry_manager_ &&
        config::ENABLE_RETRY_QUEUE) {
      PLUGIN_INFO("qq_to_tg", "消息发送失败，添加到重试队列: {} -> {}",
                  event.message_id, telegram_group_id);
      retry_manager_->add_message_retry(
          "qq", "telegram", event.message_id, message_to_send,
          telegram_group_id, qq_group_id, topic_id,
          config::MESSAGE_RETRY_MAX_ATTEMPTS, failure_reason);
    } else if (!telegram_message_id.has_value()) {
      // 如果没有启用重试或没有重试管理器，记录错误
      PLUGIN_ERROR("qq_to_tg", "消息发送失败且未启用重试: {}", failure_reason);
    }

    // 清理临时文件
    for (const std::string &temp_file : temp_files_to_cleanup) {
      MediaProcessor::cleanup_media_file(temp_file);
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "转发QQ消息到Telegram时出错: {}", e.what());
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

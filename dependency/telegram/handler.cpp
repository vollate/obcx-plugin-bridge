#include "telegram/handler.hpp"
#include "media_processor.hpp"
#include "retry_queue_manager.hpp"
#include "telegram/media_processor.hpp"
#include "telegram/message_formatter.hpp"

#include <common/logger.hpp>
#include <config.hpp>
#include <fmt/format.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

namespace bridge {

TelegramHandler::TelegramHandler(
    std::shared_ptr<storage::DatabaseManager> db_manager,
    std::shared_ptr<RetryQueueManager> retry_manager)
    : db_manager_(db_manager), retry_manager_(std::move(retry_manager)),
      media_processor_(
          std::make_unique<telegram::TelegramMediaProcessor>(db_manager)),
      command_handler_(
          std::make_unique<telegram::TelegramCommandHandler>(db_manager)),
      event_handler_(std::make_unique<telegram::TelegramEventHandler>(
          db_manager, [this](obcx::core::IBot &tg_bot, obcx::core::IBot &qq_bot,
                             obcx::common::MessageEvent event) {
            return forward_to_qq(tg_bot, qq_bot, event);
          })) {}

auto TelegramHandler::forward_to_qq(obcx::core::IBot &telegram_bot,
                                    obcx::core::IBot &qq_bot,
                                    obcx::common::MessageEvent event)
    -> boost::asio::awaitable<void> {

  // 更新Telegram平台心跳时间
  if (db_manager_) {
    db_manager_->update_platform_heartbeat("telegram",
                                           std::chrono::system_clock::now());
  }

  // 确保是群消息
  if (event.message_type != "group" || !event.group_id.has_value()) {
    co_return;
  }

  const std::string telegram_group_id = event.group_id.value();
  std::string qq_group_id;
  const GroupBridgeConfig *bridge_config = nullptr;

  // 查找对应的QQ群ID和桥接配置
  auto it = GROUP_MAP.find(telegram_group_id);
  if (it == GROUP_MAP.end()) {
    PLUGIN_DEBUG("tg_to_qq", "Telegram群 {} 没有对应的QQ群配置",
                 telegram_group_id);
    co_return;
  }
  bridge_config = &it->second;

  // 根据桥接模式处理转发逻辑
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    qq_group_id = bridge_config->qq_group_id;
    PLUGIN_DEBUG("tg_to_qq", "群组模式：Telegram群 {} 转发到QQ群 {}",
                 telegram_group_id, qq_group_id);

    // 检查是否启用TG到QQ转发
    if (!bridge_config->enable_tg_to_qq) {
      PLUGIN_DEBUG("tg_to_qq", "Telegram群 {} 到QQ群 {} 的转发已禁用，跳过",
                   telegram_group_id, qq_group_id);
      co_return;
    }
  } else {
    // Topic模式：根据topic ID查找对应的QQ群
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

    // 检查是否启用TG到QQ转发（Topic级别）
    if (!topic_config->enable_tg_to_qq) {
      PLUGIN_DEBUG("tg_to_qq", "Telegram topic {} 到QQ群 {} 的转发已禁用，跳过",
                   message_thread_id, qq_group_id);
      co_return;
    }
  }

  // 检查是否是 /recall 命令
  if (event.raw_message.starts_with("/recall")) {
    PLUGIN_INFO("tg_to_qq", "检测到 /recall 命令，处理撤回请求");
    co_await command_handler_->handle_recall_command(telegram_bot, qq_bot,
                                                     event, qq_group_id);
    co_return;
  }

  // 检查是否是 /checkalive 命令
  if (event.raw_message.starts_with("/checkalive")) {
    // 检查群组是否在配置中
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

  // 检查是否是 /poke 命令
  if (event.raw_message.starts_with("/poke")) {
    // 检查群组是否在配置中
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

  // 检查是否是编辑后的重发（通过is_edited_resend标记识别）
  bool is_edited_resend = event.data.contains("is_edited_resend") &&
                          event.data["is_edited_resend"].get<bool>();

  // 检查消息是否已转发（避免重复），但编辑重发时跳过此检查
  if (!is_edited_resend &&
      db_manager_->get_target_message_id("telegram", event.message_id, "qq")
          .has_value()) {
    PLUGIN_DEBUG("tg_to_qq", "Telegram消息 {} 已转发到QQ，跳过重复处理",
                 event.message_id);
    co_return;
  }

  PLUGIN_INFO("tg_to_qq", "准备从Telegram群 {} 转发消息到QQ群 {}",
              telegram_group_id, qq_group_id);

  // 用于收集下载的临时文件路径，以便发送后清理
  std::vector<std::string> temp_files_to_cleanup;
  std::vector<obcx::common::MessageSegment> message_to_send;

  try {
    // 保存/更新用户信息
    db_manager_->save_user_from_event(event, "telegram");

    // 保存消息信息
    db_manager_->save_message_from_event(event, "telegram");

    // 处理回复消息
    std::optional<std::string> reply_to_message_id;
    if (event.data.contains("reply_to_message")) {
      auto reply_to_message = event.data["reply_to_message"];
      if (reply_to_message.contains("message_id")) {
        std::string replied_message_id =
            std::to_string(reply_to_message["message_id"].get<int64_t>());

        // 查找被回复消息对应的QQ消息ID
        // 情况1: 如果被回复的TG消息曾经转发到QQ过，找到QQ的消息ID
        reply_to_message_id = db_manager_->get_target_message_id(
            "telegram", replied_message_id, "qq");

        // 情况2: 如果被回复的TG消息来源于QQ，找到QQ的原始消息ID
        if (!reply_to_message_id.has_value()) {
          reply_to_message_id = db_manager_->get_source_message_id(
              "telegram", replied_message_id, "qq");
        }

        // 如果最终仍未找到映射，从事件数据中移除reply_to_message以避免显示回复提示
        if (!reply_to_message_id.has_value()) {
          // 创建事件的可修改副本（JSON是可修改的）
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

    // 格式化回复消息
    telegram::TelegramMessageFormatter::format_reply_message(
        event, reply_to_message_id, message_to_send);

    // 格式化发送者信息
    telegram::TelegramMessageFormatter::format_sender_info(
        event, bridge_config, telegram_group_id, message_to_send);

    // 处理媒体文件（从message segments中提取）
    for (const auto &segment : event.message) {
      if (segment.type != "image" && segment.type != "video" &&
          segment.type != "audio" && segment.type != "voice" &&
          segment.type != "document" && segment.type != "sticker" &&
          segment.type != "animation" && segment.type != "video_note") {
        message_to_send.push_back(segment);
      } else {
        // 处理非文本消息段
        auto media_segments = co_await media_processor_->process_media_file(
            telegram_bot, segment.type, segment.data.value("file_id", ""),
            event.data, temp_files_to_cleanup);

        for (const auto &media_segment : media_segments) {
          message_to_send.push_back(media_segment);
        }
      }
    }

    // 发送消息到QQ（支持重试）
    if (!message_to_send.empty()) {
      std::optional<std::string> qq_message_id;
      std::string failure_reason;

      try {
        std::string qq_response =
            co_await qq_bot.send_group_message(qq_group_id, message_to_send);

        if (!qq_response.empty()) {
          // 解析响应获取QQ消息ID
          PLUGIN_DEBUG("tg_to_qq", "QQ API响应: {}", qq_response);
          nlohmann::json response_json = nlohmann::json::parse(qq_response);
          if (response_json.contains("status") &&
              response_json["status"] == "ok" &&
              response_json.contains("data") &&
              response_json["data"].is_object() &&
              response_json["data"].contains("message_id")) {
            qq_message_id = std::to_string(
                response_json["data"]["message_id"].get<int64_t>());

            // 保存或更新消息映射
            if (is_edited_resend) {
              // 编辑重发：更新现有映射
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
              // 首次转发：添加新映射
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

      // 如果发送失败且启用了重试队列，添加到重试队列
      if (!qq_message_id.has_value() && retry_manager_ &&
          config::ENABLE_RETRY_QUEUE) {
        PLUGIN_INFO("tg_to_qq", "消息发送失败，添加到重试队列: {} -> {}",
                    event.message_id, qq_group_id);
        retry_manager_->add_message_retry(
            "telegram", "qq", event.message_id, message_to_send, qq_group_id,
            telegram_group_id, -1, config::MESSAGE_RETRY_MAX_ATTEMPTS,
            failure_reason);
      } else if (!qq_message_id.has_value()) {
        // 如果没有启用重试或没有重试管理器，记录错误
        PLUGIN_ERROR("tg_to_qq", "消息发送失败且未启用重试: {}",
                     failure_reason);
      }
    }

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理Telegram到QQ转发时出错: {}", e.what());
  }

  // 清理临时文件
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
  // 更新Telegram平台心跳时间
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

} // namespace bridge

#include "qq/message_formatter.hpp"

#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge::qq {

QQMessageFormatter::QQMessageFormatter(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto QQMessageFormatter::format_sender_info(
    obcx::core::IBot &qq_bot, const obcx::common::MessageEvent &event,
    const GroupBridgeConfig *bridge_config, const std::string &qq_group_id,
    const std::string &telegram_group_id, int64_t topic_id,
    obcx::common::Message &message_to_send)
    -> boost::asio::awaitable<std::string> {

  std::string sender_display_name = co_await get_user_display_name(
      qq_bot, event.user_id, event.group_id.value_or(""));

  // 根据配置决定是否添加发送者信息
  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    // Topic模式：获取对应topic的配置
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  if (show_sender) {
    std::string sender_info = fmt::format("[{}]\t", sender_display_name);
    obcx::common::MessageSegment sender_segment;
    sender_segment.type = "text";
    sender_segment.data["text"] = sender_info;
    message_to_send.push_back(sender_segment);
    PLUGIN_DEBUG("qq_to_tg", "QQ到Telegram转发显示发送者：{}",
                 sender_display_name);
  } else {
    PLUGIN_DEBUG("qq_to_tg", "QQ到Telegram转发不显示发送者");
  }

  co_return sender_display_name;
}

auto QQMessageFormatter::format_reply_message(
    const obcx::common::MessageEvent &event,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<bool> {

  // 检查是否有引用消息
  std::optional<std::string> reply_message_id;
  for (const auto &segment : event.message) {
    if (segment.type == "reply") {
      // 获取被引用的QQ消息ID
      if (segment.data.contains("id")) {
        reply_message_id = segment.data.at("id");
        PLUGIN_DEBUG("qq_to_tg", "检测到QQ引用消息，引用ID: {}",
                     reply_message_id.value());
        break;
      }
    }
  }

  // 如果有引用消息，尝试查找对应平台的消息ID
  if (reply_message_id.has_value()) {
    std::optional<std::string> target_telegram_message_id;

    // 情况1: 如果被回复的QQ消息曾经转发到Telegram过，找到TG的消息ID
    target_telegram_message_id = db_manager_->get_target_message_id(
        "qq", reply_message_id.value(), "telegram");

    // 情况2: 如果被回复的QQ消息来源于Telegram，找到TG的原始消息ID
    if (!target_telegram_message_id.has_value()) {
      target_telegram_message_id = db_manager_->get_source_message_id(
          "qq", reply_message_id.value(), "telegram");
    }

    PLUGIN_DEBUG("qq_to_tg", "QQ回复消息映射查找: QQ消息ID {} -> TG消息ID {}",
                 reply_message_id.value(),
                 target_telegram_message_id.has_value()
                     ? target_telegram_message_id.value()
                     : "未找到");

    if (target_telegram_message_id.has_value()) {
      // 创建Telegram引用消息段
      obcx::common::MessageSegment reply_segment;
      reply_segment.type = "reply";
      reply_segment.data["id"] = target_telegram_message_id.value();
      message_to_send.push_back(reply_segment);
      PLUGIN_DEBUG("qq_to_tg", "添加Telegram引用消息段，引用ID: {}",
                   target_telegram_message_id.value());
      co_return true;
    } else {
      PLUGIN_DEBUG("qq_to_tg",
                   "未找到QQ引用消息对应的Telegram消息ID，可能是原生QQ消息");
    }
  }

  co_return false;
}

auto QQMessageFormatter::process_forward_message(
    obcx::core::IBot &qq_bot, obcx::core::IBot &telegram_bot,
    const obcx::common::MessageSegment &segment,
    const std::string &telegram_group_id, int64_t topic_id,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<void> {

  try {
    // 获取转发消息ID
    std::string forward_id = segment.data.value("id", "");
    if (forward_id.empty()) {
      co_return;
    }

    PLUGIN_DEBUG("qq_to_tg", "处理合并转发消息，ID: {}", forward_id);

    // 获取合并转发内容
    std::string forward_response =
        co_await static_cast<obcx::core::QQBot &>(qq_bot).get_forward_msg(
            forward_id);
    nlohmann::json forward_json = nlohmann::json::parse(forward_response);

    if (forward_json.contains("status") && forward_json["status"] == "ok" &&
        forward_json.contains("data") && forward_json["data"].is_object()) {
      auto forward_data = forward_json["data"];

      // 添加合并转发标题
      obcx::common::MessageSegment forward_title_segment;
      forward_title_segment.type = "text";
      forward_title_segment.data["text"] = "\n📋 合并转发消息:\n";
      message_to_send.push_back(forward_title_segment);

      // 收集合并转发中的所有图片
      std::vector<obcx::common::MessageSegment> forward_images;

      // 处理转发消息中的每个节点
      if (forward_data.contains("messages") &&
          forward_data["messages"].is_array()) {
        for (const auto &msg_node : forward_data["messages"]) {
          if (msg_node.is_object()) {
            // 获取发送者信息
            std::string node_sender =
                msg_node.value("sender", nlohmann::json::object())
                    .value("nickname", "未知用户");

            // 处理content数组
            std::string node_content = "";
            if (msg_node.contains("content") &&
                msg_node["content"].is_array()) {
              for (const auto &content_seg : msg_node["content"]) {
                if (content_seg.is_object() && content_seg.contains("type")) {
                  std::string seg_type = content_seg["type"];
                  if (seg_type == "text" && content_seg.contains("data") &&
                      content_seg["data"].contains("text")) {
                    node_content +=
                        content_seg["data"]["text"].get<std::string>();
                  } else if (seg_type == "face" &&
                             content_seg.contains("data") &&
                             content_seg["data"].contains("id")) {
                    node_content += fmt::format(
                        "[表情:{}]",
                        content_seg["data"]["id"].get<std::string>());
                  } else if (seg_type == "image") {
                    // 收集图片信息用于后续发送
                    node_content +=
                        fmt::format("[图片{}]", forward_images.size() + 1);

                    // 创建图片消息段
                    obcx::common::MessageSegment img_segment;
                    img_segment.type = "image";
                    if (content_seg.contains("data")) {
                      auto img_data = content_seg["data"];
                      if (img_data.contains("url") &&
                          img_data["url"].is_string()) {
                        img_segment.data["url"] =
                            img_data["url"].get<std::string>();
                        img_segment.data["file"] =
                            img_data["url"].get<std::string>();
                      } else if (img_data.contains("file") &&
                                 img_data["file"].is_string()) {
                        img_segment.data["file"] =
                            img_data["file"].get<std::string>();
                      }
                      // 复制其他可能有用的字段
                      if (img_data.contains("subType")) {
                        img_segment.data["subType"] = img_data["subType"];
                      }
                    }
                    forward_images.push_back(img_segment);
                    PLUGIN_DEBUG(
                        "qq_to_tg", "收集合并转发中的图片: url={}",
                        img_segment.data.value(
                            "url", img_segment.data.value("file", "无URL")));
                  } else if (seg_type == "at" && content_seg.contains("data") &&
                             content_seg["data"].contains("qq")) {
                    node_content += fmt::format(
                        "[@{}]", content_seg["data"]["qq"].get<std::string>());
                  } else {
                    node_content += fmt::format("[{}]", seg_type);
                  }
                }
              }
            } else if (msg_node.contains("content") &&
                       msg_node["content"].is_string()) {
              // 兼容字符串格式的content
              node_content = msg_node["content"].get<std::string>();
            }

            // 添加每个转发消息的内容
            obcx::common::MessageSegment node_segment;
            node_segment.type = "text";
            node_segment.data["text"] =
                fmt::format("👤 {}: {}\n", node_sender, node_content);
            message_to_send.push_back(node_segment);
          }
        }
      }

      // 如果收集到了图片，使用sendMediaGroup批量发送（每批最多10张）
      if (!forward_images.empty()) {
        PLUGIN_INFO("qq_to_tg",
                    "合并转发消息中发现 {} 张图片，准备使用MediaGroup发送",
                    forward_images.size());

        // 构建完整的媒体组列表 (type, url)
        std::vector<std::pair<std::string, std::string>> all_media;
        for (const auto &img_seg : forward_images) {
          std::string url =
              img_seg.data.value("url", img_seg.data.value("file", ""));
          if (!url.empty()) {
            all_media.emplace_back("photo", url);
            PLUGIN_DEBUG("qq_to_tg", "添加图片到MediaGroup: {}", url);
          }
        }

        // 分批发送（每批最多10张）
        if (!all_media.empty()) {
          std::optional<int64_t> opt_topic_id =
              (topic_id == -1) ? std::nullopt
                               : std::optional<int64_t>(topic_id);
          size_t total_batches = (all_media.size() + 9) / 10;
          size_t sent_count = 0;

          for (size_t batch = 0; batch < total_batches; ++batch) {
            size_t batch_start = batch * 10;
            size_t batch_size = std::min(static_cast<size_t>(10),
                                         all_media.size() - batch_start);

            std::vector<std::pair<std::string, std::string>> batch_media(
                all_media.begin() + batch_start,
                all_media.begin() + batch_start + batch_size);

            try {
              std::string caption = fmt::format(
                  "📸 合并转发消息中的图片 ({}/{})", batch + 1, total_batches);

              std::string media_response =
                  co_await static_cast<obcx::core::TGBot &>(telegram_bot)
                      .send_media_group(telegram_group_id, batch_media, caption,
                                        opt_topic_id, std::nullopt);

              PLUGIN_INFO("qq_to_tg",
                          "成功通过MediaGroup发送第 {}/{} 批 {} 张图片",
                          batch + 1, total_batches, batch_media.size());
              sent_count += batch_media.size();
            } catch (const std::exception &e) {
              PLUGIN_ERROR("qq_to_tg",
                           "通过MediaGroup发送第 {}/{} 批图片失败: {}",
                           batch + 1, total_batches, e.what());
              // 失败时添加错误提示到文本消息
              obcx::common::MessageSegment error_segment;
              error_segment.type = "text";
              error_segment.data["text"] =
                  fmt::format("\n[发送第{}/{}批{}张图片失败: {}]", batch + 1,
                              total_batches, batch_media.size(), e.what());
              message_to_send.push_back(error_segment);
            }
          }

          if (sent_count > 0) {
            PLUGIN_INFO("qq_to_tg",
                        "合并转发消息图片发送完成，共成功发送 {}/{} 张",
                        sent_count, all_media.size());
          }
        }
      }

      PLUGIN_INFO(
          "qq_to_tg", "成功处理合并转发消息，包含 {} 条消息，{} 张图片",
          forward_data.value("messages", nlohmann::json::array()).size(),
          forward_images.size());
    } else {
      PLUGIN_WARN("qq_to_tg", "获取合并转发内容失败: {}", forward_response);
      // 添加失败提示
      obcx::common::MessageSegment error_segment;
      error_segment.type = "text";
      error_segment.data["text"] = "[合并转发消息获取失败]";
      message_to_send.push_back(error_segment);
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理合并转发消息时出错: {}", e.what());
    // 添加错误提示
    obcx::common::MessageSegment error_segment;
    error_segment.type = "text";
    error_segment.data["text"] = "[合并转发消息处理失败]";
    message_to_send.push_back(error_segment);
  }
}

auto QQMessageFormatter::process_node_message(
    const obcx::common::MessageSegment &segment,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<void> {

  try {
    // node段包含用户ID、昵称和内容
    std::string node_user_id = segment.data.value("user_id", "");
    std::string node_nickname = segment.data.value("nickname", "未知用户");

    // 内容可能是字符串或消息段数组
    if (segment.data.contains("content")) {
      auto content = segment.data.at("content");

      obcx::common::MessageSegment node_segment;
      node_segment.type = "text";

      if (content.is_string()) {
        // 简单文本内容
        node_segment.data["text"] = fmt::format("👤 {}: {}\n", node_nickname,
                                                content.get<std::string>());
      } else if (content.is_array()) {
        // 复杂消息段内容
        std::string node_text = fmt::format("👤 {}: ", node_nickname);
        for (const auto &content_segment : content) {
          if (content_segment.is_object() && content_segment.contains("type")) {
            std::string seg_type = content_segment["type"];
            if (seg_type == "text" && content_segment.contains("data") &&
                content_segment["data"].contains("text")) {
              node_text += content_segment["data"]["text"].get<std::string>();
            } else if (seg_type == "face") {
              node_text += fmt::format(
                  "[表情:{}]",
                  content_segment.value("data", nlohmann::json::object())
                      .value("id", "0"));
            } else if (seg_type == "image") {
              node_text += "[图片]";
            } else {
              node_text += fmt::format("[{}]", seg_type);
            }
          }
        }
        node_text += "\n";
        node_segment.data["text"] = node_text;
      } else {
        node_segment.data["text"] =
            fmt::format("👤 {}: [未知内容]\n", node_nickname);
      }

      message_to_send.push_back(node_segment);
      PLUGIN_DEBUG("qq_to_tg", "处理node消息段: 用户 {} ({})", node_nickname,
                   node_user_id);
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理node消息段时出错: {}", e.what());
    obcx::common::MessageSegment error_segment;
    error_segment.type = "text";
    error_segment.data["text"] = "[转发节点处理失败]";
    message_to_send.push_back(error_segment);
  }

  co_return;
}

auto QQMessageFormatter::send_media_group(
    obcx::core::IBot &telegram_bot,
    const std::vector<obcx::common::MessageSegment> &image_segments,
    const std::vector<obcx::common::MessageSegment> &other_segments,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config,
    const obcx::common::Message &message_to_send,
    const obcx::common::MessageEvent &event) -> boost::asio::awaitable<bool> {

  if (image_segments.size() <= 1) {
    co_return false; // 不需要MediaGroup
  }

  PLUGIN_INFO("qq_to_tg", "检测到多张图片({})，使用MediaGroup发送",
              image_segments.size());

  bool any_batch_sent = false;
  for (size_t sent_count = 0; sent_count < image_segments.size();
       sent_count += 10) {
    // 计算这一批次应该发送多少张图片（最多10张）
    size_t batch_size =
        std::min(static_cast<size_t>(10), image_segments.size() - sent_count);
    PLUGIN_DEBUG("qq_to_tg",
                 "准备发送MediaGroup图片，起始索引: {}, 本批次数量: {}",
                 sent_count, batch_size);
    // 构建媒体组列表 (type, url)
    std::vector<std::pair<std::string, std::string>> media_list;
    for (size_t i = 0; i < batch_size; ++i) {
      std::string url = image_segments[sent_count + i].data.value(
          "url", image_segments[sent_count + i].data.value("file", ""));
      if (!url.empty()) {
        media_list.emplace_back("photo", url);
        PLUGIN_DEBUG("qq_to_tg", "添加图片到MediaGroup: {}", url);
      }
    }

    // 如果有有效的图片URL，使用sendMediaGroup发送
    if (!media_list.empty()) {
      try {
        // 收集文本内容作为caption
        std::string caption;

        // 根据配置决定是否显示发送者
        bool show_sender = false;
        if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
          show_sender = bridge_config->show_qq_to_tg_sender;
        } else {
          const TopicBridgeConfig *topic_config =
              bridge::get_topic_config(telegram_group_id, topic_id);
          show_sender =
              topic_config ? topic_config->show_qq_to_tg_sender : false;
        }

        if (show_sender) {
          caption = fmt::format("[{}]", sender_display_name);
        }

        // 添加其他文本段的内容到caption
        for (const auto &seg : other_segments) {
          if (seg.type == "text" && seg.data.contains("text")) {
            if (!caption.empty()) {
              caption += "\n";
            }
            caption += seg.data.at("text");
          }
        }

        std::optional<int64_t> opt_topic_id =
            (topic_id == -1) ? std::nullopt : std::optional<int64_t>(topic_id);

        // 获取回复消息ID（如果有）
        std::optional<std::string> opt_reply_id;
        for (const auto &seg : message_to_send) {
          if (seg.type == "reply" && seg.data.contains("id")) {
            opt_reply_id = seg.data.at("id");
            break;
          }
        }

        std::string media_response =
            co_await static_cast<obcx::core::TGBot &>(telegram_bot)
                .send_media_group(telegram_group_id, media_list, caption,
                                  opt_topic_id, opt_reply_id);

        PLUGIN_INFO("qq_to_tg", "成功通过MediaGroup发送 {} 张图片",
                    media_list.size());

        // 解析响应获取消息ID用于映射
        if (!media_response.empty()) {
          try {
            nlohmann::json response_json =
                nlohmann::json::parse(media_response);
            if (response_json.contains("result") &&
                response_json["result"].is_array() &&
                !response_json["result"].empty()) {
              // 使用第一个消息的ID作为映射
              auto first_msg = response_json["result"][0];
              if (first_msg.contains("message_id")) {
                std::string tg_msg_id =
                    std::to_string(first_msg["message_id"].get<int64_t>());
                storage::MessageMapping mapping;
                mapping.source_platform = "qq";
                mapping.source_message_id = event.message_id;
                mapping.target_platform = "telegram";
                mapping.target_message_id = tg_msg_id;
                mapping.created_at = std::chrono::system_clock::now();
                db_manager_->add_message_mapping(mapping);
                PLUGIN_DEBUG("qq_to_tg",
                             "记录MediaGroup消息映射: QQ {} -> TG {}",
                             event.message_id, tg_msg_id);
              }
            }
          } catch (const std::exception &e) {
            PLUGIN_WARN("qq_to_tg", "解析MediaGroup响应失败: {}", e.what());
          }
        }

        any_batch_sent = true;
      } catch (const std::exception &e) {
        PLUGIN_ERROR("qq_to_tg",
                     "通过MediaGroup发送图片失败: {}，回退到单图发送",
                     e.what());
        co_return false; // 发送失败，回退到单图发送模式
      }
    }
  }
  co_return any_batch_sent;
}

auto QQMessageFormatter::get_user_display_name(obcx::core::IBot &qq_bot,
                                               const std::string &user_id,
                                               const std::string &group_id)
    -> boost::asio::awaitable<std::string> {

  auto display_name =
      db_manager_->query_user_display_name("qq", user_id, group_id);

  if (!display_name.has_value()) {
    co_await fetch_user_info(qq_bot, user_id, group_id);
    display_name =
        db_manager_->query_user_display_name("qq", user_id, group_id);
  }

  co_return display_name.value_or(user_id);
}

auto QQMessageFormatter::fetch_user_info(obcx::core::IBot &qq_bot,
                                         const std::string &user_id,
                                         const std::string &group_id)
    -> boost::asio::awaitable<void> {
  co_await fetch_and_save_user_info(db_manager_, qq_bot, user_id, group_id);
}

auto QQMessageFormatter::fetch_and_save_user_info(
    std::shared_ptr<storage::DatabaseManager> db_manager,
    obcx::core::IBot &qq_bot, const std::string &user_id,
    const std::string &group_id) -> boost::asio::awaitable<void> {
  try {
    std::string response =
        co_await qq_bot.get_group_member_info(group_id, user_id, false);
    nlohmann::json response_json = nlohmann::json::parse(response);

    PLUGIN_DEBUG("qq_to_tg", "QQ群成员信息API响应: {}", response);

    if (response_json.contains("status") && response_json["status"] == "ok" &&
        response_json.contains("data") && response_json["data"].is_object()) {

      auto data = response_json["data"];
      PLUGIN_DEBUG("qq_to_tg", "QQ群成员信息详细数据: {}", data.dump());

      storage::UserInfo user_info;
      user_info.platform = "qq";
      user_info.user_id = user_id;
      user_info.group_id = group_id; // 群组特定的用户信息
      user_info.last_updated = std::chrono::system_clock::now();

      std::string general_nickname, card, title;

      if (data.contains("nickname") && data["nickname"].is_string()) {
        general_nickname = data["nickname"];
      }

      if (data.contains("card") && data["card"].is_string()) {
        card = data["card"];
      }

      if (data.contains("title") && data["title"].is_string()) {
        title = data["title"];
      }

      // 优先级：群名片 > 群头衔 > 一般昵称
      // 将最优先的名称存储在nickname字段中，便于显示逻辑处理
      if (!card.empty()) {
        user_info.nickname = card;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ群名片作为显示名称: {} -> {}", user_id,
                     card);
      } else if (!title.empty()) {
        user_info.nickname = title;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ群头衔作为显示名称: {} -> {}", user_id,
                     title);
      } else if (!general_nickname.empty()) {
        user_info.nickname = general_nickname;
        PLUGIN_DEBUG("qq_to_tg", "使用QQ一般昵称作为显示名称: {} -> {}",
                     user_id, general_nickname);
      }

      // 同时保存群头衔到title字段供后续使用
      if (!title.empty()) {
        user_info.title = title;
      }

      // 保存用户信息
      db_manager->save_or_update_user(user_info, true);
      PLUGIN_DEBUG("qq_to_tg", "获取QQ用户信息成功：{} -> {}", user_id,
                   user_info.nickname);
    }
  } catch (const std::exception &e) {
    PLUGIN_DEBUG("qq_to_tg", "获取QQ用户信息失败：{}", e.what());
  }
}

} // namespace bridge::qq

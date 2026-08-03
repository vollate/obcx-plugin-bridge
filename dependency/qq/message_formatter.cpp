#include "qq/message_formatter.hpp"

#include "bridge_state_repository.hpp"
#include "qq/image_url_validator.hpp"

#include <common/json_utils.hpp>
#include <common/logger.hpp>
#include <fmt/format.h>
#include <interfaces/qq_bot.hpp>
#include <interfaces/telegram_bot.hpp>
#include <nlohmann/json.hpp>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace bridge::qq {

namespace {

namespace fs = std::filesystem;

class TemporaryMediaUploads {
public:
  TemporaryMediaUploads() = default;
  TemporaryMediaUploads(const TemporaryMediaUploads &) = delete;
  auto operator=(const TemporaryMediaUploads &)
      -> TemporaryMediaUploads & = delete;

  TemporaryMediaUploads(TemporaryMediaUploads &&other) noexcept
      : root_(std::move(other.root_)), uploads_(std::move(other.uploads_)) {
    other.root_.clear();
  }

  auto operator=(TemporaryMediaUploads &&other) noexcept
      -> TemporaryMediaUploads & {
    if (this != &other) {
      cleanup();
      root_ = std::move(other.root_);
      uploads_ = std::move(other.uploads_);
      other.root_.clear();
    }
    return *this;
  }

  ~TemporaryMediaUploads() { cleanup(); }

  static auto materialize(std::vector<DownloadedImage> downloaded)
      -> TemporaryMediaUploads {
    TemporaryMediaUploads result;
    result.root_ = fs::temp_directory_path() /
                   ("obcx-qq-media-" + boost::uuids::to_string(
                                           boost::uuids::random_generator{}()));
    std::error_code error;
    const bool created = fs::create_directory(result.root_, error);
    if (error || !created) {
      throw std::runtime_error("cannot create QQ media temporary directory");
    }

    result.uploads_.reserve(downloaded.size());
    for (auto &image : downloaded) {
      auto filename = fs::path{image.filename}.filename().string();
      if (filename.empty()) {
        throw std::runtime_error("QQ media temporary filename is empty");
      }
      const auto path = result.root_ / filename;
      {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(image.data.data(),
                     static_cast<std::streamsize>(image.data.size()));
        if (!output) {
          throw std::runtime_error("cannot write QQ media temporary file");
        }
      }

      std::ifstream input(path, std::ios::binary);
      std::string file_data{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
      if (input.bad() || file_data.size() != image.data.size()) {
        throw std::runtime_error("cannot read QQ media temporary file");
      }
      result.uploads_.push_back(obcx::core::TelegramMediaUpload{
          .type = std::move(image.type),
          .filename = std::move(filename),
          .mime_type = std::move(image.mime_type),
          .data = std::move(file_data),
      });
    }
    return result;
  }

  [[nodiscard]] auto uploads() const
      -> const std::vector<obcx::core::TelegramMediaUpload> & {
    return uploads_;
  }

  [[nodiscard]] auto root() const -> const fs::path & { return root_; }

  void mark_cleaned() noexcept { root_.clear(); }

private:
  void cleanup() noexcept {
    if (root_.empty()) {
      return;
    }
    std::error_code ignored;
    fs::remove_all(root_, ignored);
    root_.clear();
  }

  fs::path root_;
  std::vector<obcx::core::TelegramMediaUpload> uploads_;
};

auto is_telegram_bad_request(std::string_view error) -> bool {
  return error.find("HTTP request failed: 400") != std::string_view::npos;
}

} // namespace

QQMessageFormatter::QQMessageFormatter(
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
    ImageDownloader image_downloader)
    : config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)),
      image_downloader_(std::move(image_downloader)) {
  if (!image_downloader_) {
    image_downloader_ =
        [](const bridge::BridgeConfig &config,
           const std::vector<std::pair<std::string, std::string>> &media) {
          return ImageUrlValidator::download(config, media);
        };
  }
}

auto QQMessageFormatter::send_media_group_with_fallback(
    obcx::core::IBot &telegram_bot, std::string_view telegram_group_id,
    const std::vector<std::pair<std::string, std::string>> &media,
    std::string_view caption, std::optional<int64_t> topic_id,
    std::optional<std::string> reply_to_message_id, bool &used_multipart)
    -> boost::asio::awaitable<std::string> {
  used_multipart = false;
  auto &telegram = dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);
  try {
    co_return co_await telegram.send_media_group(
        telegram_group_id, media, caption, topic_id, reply_to_message_id);
  } catch (const std::exception &error) {
    if (!is_telegram_bad_request(error.what())) {
      throw;
    }
    OBCX_WARN("MediaGroup URL 发送被 Telegram 拒绝 ({})，"
              "下载媒体并改用 multipart 上传",
              error.what());
  }

  if (!blocking_executor_) {
    throw std::runtime_error(
        "multipart media fallback requires the runtime blocking executor");
  }
  auto *uploader =
      dynamic_cast<obcx::core::ITelegramMediaGroupUploader *>(&telegram_bot);
  if (uploader == nullptr) {
    throw std::runtime_error(
        "Telegram bot does not support multipart media group uploads");
  }

  auto downloaded = co_await image_downloader_(*config_, media);
  auto temporary = co_await blocking_executor_->run(
      [downloaded = std::move(downloaded)]() mutable {
        return TemporaryMediaUploads::materialize(std::move(downloaded));
      });
  const auto temporary_root = temporary.root();

  std::string response;
  std::exception_ptr upload_error;
  try {
    response = co_await uploader->send_media_group_uploads(
        telegram_group_id, temporary.uploads(), caption, topic_id,
        reply_to_message_id);
    used_multipart = true;
  } catch (...) {
    upload_error = std::current_exception();
  }

  const auto cleanup_error = co_await blocking_executor_->run([temporary_root] {
    std::error_code error;
    fs::remove_all(temporary_root, error);
    return error ? error.message() : std::string{};
  });
  if (cleanup_error.empty()) {
    temporary.mark_cleaned();
    OBCX_TRACE("已删除 QQ MediaGroup 临时目录: {}", temporary_root.string());
  } else {
    OBCX_WARN("删除 QQ MediaGroup 临时目录失败，将在析构时重试: {} ({})",
              temporary_root.string(), cleanup_error);
  }

  if (upload_error) {
    std::rethrow_exception(upload_error);
  }
  co_return response;
}

auto QQMessageFormatter::format_sender_info(
    obcx::core::IBot &qq_bot, const obcx::common::MessageEvent &event,
    const GroupBridgeConfig *bridge_config, const std::string &qq_group_id,
    const std::string &telegram_group_id, int64_t topic_id,
    obcx::common::Message &message_to_send)
    -> boost::asio::awaitable<std::string> {

  std::string sender_display_name = co_await get_user_display_name(
      qq_bot, event.user_id, event.group_id.value_or(""));

  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    const TopicBridgeConfig *topic_config =
        config_->topic_config(telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  if (show_sender) {
    std::string sender_info = fmt::format("[{}]\t", sender_display_name);
    obcx::common::MessageSegment sender_segment;
    sender_segment.type = "text";
    sender_segment.data["text"] = sender_info;
    message_to_send.push_back(sender_segment);
    OBCX_DEBUG("QQ到Telegram转发显示发送者：{}", sender_display_name);
  } else {
    OBCX_DEBUG("QQ到Telegram转发不显示发送者");
  }

  co_return sender_display_name;
}

auto QQMessageFormatter::format_reply_message(
    const obcx::common::MessageEvent &event,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<bool> {

  std::optional<std::string> reply_message_id;
  for (const auto &segment : event.message) {
    if (segment.type == "reply") {
      reply_message_id = obcx::common::JsonUtils::get_optional_id_as_string(
          segment.data, "id");
      if (reply_message_id.has_value()) {
        OBCX_DEBUG("检测到QQ引用消息，引用ID: {}", reply_message_id.value());
        break;
      }
    }
  }

  if (reply_message_id.has_value()) {
    std::optional<std::string> target_telegram_message_id;

    // 被回复的 QQ 消息可能有两种来源：原生 QQ 消息（之前转发到 TG 过），
    // 或本身是从 TG 转发来的。先查 qq->tg 映射，没命中再查 tg 原始消息。
    if (state_repository_) {
      target_telegram_message_id = co_await blocking_executor_->run(
          [repository = state_repository_,
           reply_message_id = *reply_message_id] {
            auto target = repository->get_target_message_id(
                "qq", reply_message_id, "telegram");
            return target.has_value() ? target
                                      : repository->get_source_message_id(
                                            "qq", reply_message_id, "telegram");
          });
    }

    OBCX_DEBUG("QQ回复消息映射查找: QQ消息ID {} -> TG消息ID {}",
               reply_message_id.value(),
               target_telegram_message_id.has_value()
                   ? target_telegram_message_id.value()
                   : "未找到");

    if (target_telegram_message_id.has_value()) {
      obcx::common::MessageSegment reply_segment;
      reply_segment.type = "reply";
      reply_segment.data["id"] = target_telegram_message_id.value();
      message_to_send.push_back(reply_segment);
      OBCX_DEBUG("添加Telegram引用消息段，引用ID: {}",
                 target_telegram_message_id.value());
      co_return true;
    } else {
      OBCX_DEBUG("未找到QQ引用消息对应的Telegram消息ID，可能是原生QQ消息");
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
    std::string forward_id = segment.data.value("id", "");
    if (forward_id.empty()) {
      co_return;
    }

    OBCX_DEBUG("处理合并转发消息，ID: {}", forward_id);

    std::string forward_response =
        co_await dynamic_cast<obcx::core::IQQBot &>(qq_bot).get_forward_msg(
            forward_id);
    nlohmann::json forward_json = nlohmann::json::parse(forward_response);

    if (forward_json.contains("status") && forward_json["status"] == "ok" &&
        forward_json.contains("data") && forward_json["data"].is_object()) {
      auto forward_data = forward_json["data"];

      obcx::common::MessageSegment forward_title_segment;
      forward_title_segment.type = "text";
      forward_title_segment.data["text"] = "\n📋 合并转发消息:\n";
      message_to_send.push_back(forward_title_segment);

      // 合并转发节点里的图片要单独走 sendMediaGroup，不能塞进文本里。
      std::vector<obcx::common::MessageSegment> forward_images;

      if (forward_data.contains("messages") &&
          forward_data["messages"].is_array()) {
        for (const auto &msg_node : forward_data["messages"]) {
          if (msg_node.is_object()) {
            std::string node_sender =
                msg_node.value("sender", nlohmann::json::object())
                    .value("nickname", "未知用户");

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
                    // 占位文字：图片本体在收集后批量发送，节点文本里只保留序号引用。
                    node_content +=
                        fmt::format("[图片{}]", forward_images.size() + 1);

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
                      if (img_data.contains("subType")) {
                        img_segment.data["subType"] = img_data["subType"];
                      }
                    }
                    forward_images.push_back(img_segment);
                    OBCX_DEBUG(
                        "收集合并转发中的图片: url={}",
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
              // 兼容老版本 content 直接是字符串的格式
              node_content = msg_node["content"].get<std::string>();
            }

            obcx::common::MessageSegment node_segment;
            node_segment.type = "text";
            node_segment.data["text"] =
                fmt::format("👤 {}: {}\n", node_sender, node_content);
            message_to_send.push_back(node_segment);
          }
        }
      }

      // 合并转发收集到的图片同样走 sendMediaGroup（每批最多10张）。
      if (!forward_images.empty()) {
        OBCX_INFO("合并转发消息中发现 {} 张图片，准备使用MediaGroup发送",
                  forward_images.size());

        std::vector<std::pair<std::string, std::string>> all_media;
        for (const auto &img_seg : forward_images) {
          std::string url =
              img_seg.data.value("url", img_seg.data.value("file", ""));
          if (!url.empty()) {
            all_media.emplace_back("photo", url);
            OBCX_DEBUG("添加图片到MediaGroup: {}", url);
          }
        }

        if (!all_media.empty()) {
          std::optional<int64_t> opt_topic_id =
              (topic_id == -1) ? std::nullopt
                               : std::optional<int64_t>(topic_id);
          size_t total_batches = (all_media.size() + 9) / 10;
          size_t sent_count = 0;
          size_t total_replaced_count = 0;

          size_t batch_start = 0;
          for (size_t batch = 0; batch < total_batches; ++batch) {
            const size_t remaining = all_media.size() - batch_start;
            // Telegram media groups require at least two items. When eleven
            // remain, send 9 + 2 instead of 10 + 1.
            const size_t batch_size =
                remaining == 11 ? 9
                                : std::min(static_cast<size_t>(10), remaining);

            std::vector<std::pair<std::string, std::string>> batch_media(
                all_media.begin() + batch_start,
                all_media.begin() + batch_start + batch_size);

            // 同样地，对合并转发里的图片做一次预校验，避免某一张挂掉拖垮整批。
            // 不可达的 URL 一律替换为占位图，所以批次大小始终保持不变。
            std::vector<std::string> replaced;
            batch_media = co_await ImageUrlValidator::sanitize(
                *config_, batch_media, replaced);
            total_replaced_count += replaced.size();

            try {
              std::string caption = fmt::format(
                  "📸 合并转发消息中的图片 ({}/{})", batch + 1, total_batches);
              if (!replaced.empty()) {
                caption +=
                    fmt::format("\n⚠️ {} 张图片暂时无法获取，已用占位图替换",
                                replaced.size());
              }

              bool used_multipart = false;
              const auto media_response =
                  co_await send_media_group_with_fallback(
                      telegram_bot, telegram_group_id, batch_media, caption,
                      opt_topic_id, std::nullopt, used_multipart);

              OBCX_INFO("成功通过MediaGroup发送第 {}/{} 批 {} 张图片{}",
                        batch + 1, total_batches, batch_media.size(),
                        used_multipart ? "（multipart兜底）" : "");
              sent_count += batch_media.size();
            } catch (const std::exception &e) {
              OBCX_ERROR("合并转发 MediaGroup 第 {}/{} 批 URL 与 multipart "
                         "发送均失败: {}，本批放弃",
                         batch + 1, total_batches, e.what());
              obcx::common::MessageSegment error_segment;
              error_segment.type = "text";
              error_segment.data["text"] =
                  fmt::format("\n[发送第{}/{}批{}张图片失败: {}]", batch + 1,
                              total_batches, batch_media.size(), e.what());
              message_to_send.push_back(error_segment);
            }
            batch_start += batch_size;
          }

          if (sent_count > 0) {
            OBCX_INFO("合并转发消息图片发送完成，共成功发送 {}/{} 张 "
                      "(其中 {} 张失败已用占位图替换)",
                      sent_count, all_media.size(), total_replaced_count);
          }
        }
      }

      OBCX_INFO("成功处理合并转发消息，包含 {} 条消息，{} 张图片",
                forward_data.value("messages", nlohmann::json::array()).size(),
                forward_images.size());
    } else {
      OBCX_WARN("获取合并转发内容失败: {}", forward_response);
      obcx::common::MessageSegment error_segment;
      error_segment.type = "text";
      error_segment.data["text"] = "[合并转发消息获取失败]";
      message_to_send.push_back(error_segment);
    }
  } catch (const std::exception &e) {
    OBCX_ERROR("处理合并转发消息时出错: {}", e.what());
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
    std::string node_user_id = segment.data.value("user_id", "");
    std::string node_nickname = segment.data.value("nickname", "未知用户");

    if (segment.data.contains("content")) {
      auto content = segment.data.at("content");

      obcx::common::MessageSegment node_segment;
      node_segment.type = "text";

      if (content.is_string()) {
        node_segment.data["text"] = fmt::format("👤 {}: {}\n", node_nickname,
                                                content.get<std::string>());
      } else if (content.is_array()) {
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
      OBCX_DEBUG("处理node消息段: 用户 {} ({})", node_nickname, node_user_id);
    }
  } catch (const std::exception &e) {
    OBCX_ERROR("处理node消息段时出错: {}", e.what());
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
    const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<MediaGroupSendResult> {

  MediaGroupSendResult result;

  if (image_segments.size() <= 1) {
    co_return result;
  }

  OBCX_INFO("检测到多张图片({})，使用MediaGroup发送", image_segments.size());

  // 累积所有批次中被占位图替换掉的 URL，仅在最后一批的 caption 末尾追加一次
  // 提示，避免每批都重复一遍。
  std::vector<std::string> total_replaced;
  for (size_t sent_count = 0; sent_count < image_segments.size();) {
    const size_t remaining = image_segments.size() - sent_count;
    // Keep a trailing item paired: 11 becomes 9 + 2, never 10 + 1.
    const size_t batch_size =
        remaining == 11 ? 9 : std::min(static_cast<size_t>(10), remaining);
    OBCX_DEBUG("准备发送MediaGroup图片，起始索引: {}, 本批次数量: {}",
               sent_count, batch_size);
    std::vector<std::pair<std::string, std::string>> media_list;
    for (size_t i = 0; i < batch_size; ++i) {
      std::string url = image_segments[sent_count + i].data.value(
          "url", image_segments[sent_count + i].data.value("file", ""));
      if (!url.empty()) {
        media_list.emplace_back("photo", url);
        OBCX_DEBUG("添加图片到MediaGroup: {}", url);
      }
    }

    // 先做可达性探测：单张 5xx/超时可能让整批 sendMediaGroup 失败，所以
    // 在送给 Telegram 前把不可达 URL 替换成占位图，保持批大小不变。
    std::vector<std::string> replaced;
    if (!media_list.empty()) {
      media_list =
          co_await ImageUrlValidator::sanitize(*config_, media_list, replaced);
      total_replaced.insert(total_replaced.end(), replaced.begin(),
                            replaced.end());
    }

    if (!media_list.empty()) {
      try {
        std::string caption;

        bool show_sender = false;
        if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
          show_sender = bridge_config->show_qq_to_tg_sender;
        } else {
          const TopicBridgeConfig *topic_config =
              config_->topic_config(telegram_group_id, topic_id);
          show_sender =
              topic_config ? topic_config->show_qq_to_tg_sender : false;
        }

        if (show_sender) {
          caption = fmt::format("[{}]", sender_display_name);
        }

        for (const auto &seg : other_segments) {
          if (seg.type == "text" && seg.data.contains("text")) {
            if (!caption.empty()) {
              caption += "\n";
            }
            caption += seg.data.at("text");
          }
        }

        // 仅在最后一批 caption 末尾追加替换提示。
        const bool is_last_batch =
            sent_count + batch_size >= image_segments.size();
        if (is_last_batch && !total_replaced.empty()) {
          if (!caption.empty()) {
            caption += "\n";
          }
          caption += fmt::format("⚠️ {} 张图片暂时无法获取，已用占位图替换",
                                 total_replaced.size());
        }

        std::optional<int64_t> opt_topic_id =
            (topic_id == -1) ? std::nullopt : std::optional<int64_t>(topic_id);

        std::optional<std::string> opt_reply_id;
        for (const auto &seg : message_to_send) {
          if (seg.type == "reply") {
            opt_reply_id = obcx::common::JsonUtils::get_optional_id_as_string(
                seg.data, "id");
            if (opt_reply_id.has_value()) {
              break;
            }
          }
        }

        bool used_multipart = false;
        const auto media_response = co_await send_media_group_with_fallback(
            telegram_bot, telegram_group_id, media_list, caption, opt_topic_id,
            opt_reply_id, used_multipart);

        OBCX_INFO("成功通过MediaGroup发送 {} 张图片{}", media_list.size(),
                  used_multipart ? "（multipart兜底）" : "");

        if (!media_response.empty()) {
          try {
            nlohmann::json response_json =
                nlohmann::json::parse(media_response);
            if (response_json.contains("result") &&
                response_json["result"].is_array() &&
                !response_json["result"].empty()) {
              // MediaGroup 会返回多条消息ID，这里只用第一条建立映射。
              auto first_msg = response_json["result"][0];
              if (first_msg.contains("message_id")) {
                std::string tg_msg_id =
                    std::to_string(first_msg["message_id"].get<int64_t>());
                if (!result.primary_target_message_id.has_value()) {
                  result.primary_target_message_id = tg_msg_id;
                }
                OBCX_DEBUG("MediaGroup主消息: QQ {} -> TG {}", event.message_id,
                           tg_msg_id);
              }
            }
          } catch (const std::exception &e) {
            OBCX_WARN("解析MediaGroup响应失败: {}", e.what());
          }
        }

        result.sent = true;
      } catch (const std::exception &e) {
        OBCX_ERROR("MediaGroup URL 与 multipart 发送均失败: {}，本批放弃",
                   e.what());
      }
    }
    sent_count += batch_size;
  }
  co_return result;
}

auto QQMessageFormatter::get_user_display_name(obcx::core::IBot &qq_bot,
                                               const std::string &user_id,
                                               const std::string &group_id)
    -> boost::asio::awaitable<std::string> {

  std::optional<std::string> display_name;
  if (state_repository_) {
    display_name = co_await blocking_executor_->run(
        [repository = state_repository_, user_id, group_id] {
          return repository->query_user_display_name("qq", user_id, group_id);
        });
  }

  if (!display_name.has_value()) {
    co_await fetch_user_info(qq_bot, user_id, group_id);
    if (state_repository_) {
      display_name = co_await blocking_executor_->run(
          [repository = state_repository_, user_id, group_id] {
            return repository->query_user_display_name("qq", user_id, group_id);
          });
    }
  }

  co_return display_name.value_or(user_id);
}

auto QQMessageFormatter::fetch_user_info(obcx::core::IBot &qq_bot,
                                         const std::string &user_id,
                                         const std::string &group_id)
    -> boost::asio::awaitable<void> {
  co_await fetch_and_save_user_info(state_repository_, blocking_executor_,
                                    qq_bot, user_id, group_id);
}

auto QQMessageFormatter::fetch_and_save_user_info(
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
    obcx::core::IBot &qq_bot, const std::string &user_id,
    const std::string &group_id) -> boost::asio::awaitable<void> {
  try {
    std::string response =
        co_await qq_bot.get_group_member_info(group_id, user_id, false);
    nlohmann::json response_json = nlohmann::json::parse(response);

    OBCX_DEBUG("QQ群成员信息API响应: {}", response);

    if (response_json.contains("status") && response_json["status"] == "ok" &&
        response_json.contains("data") && response_json["data"].is_object()) {

      auto data = response_json["data"];
      OBCX_DEBUG("QQ群成员信息详细数据: {}", data.dump());

      storage::UserInfo user_info;
      user_info.platform = "qq";
      user_info.user_id = user_id;
      user_info.group_id = group_id;
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

      // 显示名优先级：群名片 > 群头衔 > 一般昵称
      if (!card.empty()) {
        user_info.nickname = card;
        OBCX_DEBUG("使用QQ群名片作为显示名称: {} -> {}", user_id, card);
      } else if (!title.empty()) {
        user_info.nickname = title;
        OBCX_DEBUG("使用QQ群头衔作为显示名称: {} -> {}", user_id, title);
      } else if (!general_nickname.empty()) {
        user_info.nickname = general_nickname;
        OBCX_DEBUG("使用QQ一般昵称作为显示名称: {} -> {}", user_id,
                   general_nickname);
      }

      // title 单独保留一份，供需要群头衔的逻辑使用。
      if (!title.empty()) {
        user_info.title = title;
      }

      if (state_repository) {
        (void)co_await blocking_executor->run([state_repository, user_info] {
          return state_repository->save_or_update_user(user_info, true);
        });
      }
      OBCX_DEBUG("获取QQ用户信息成功：{} -> {}", user_id, user_info.nickname);
    }
  } catch (const std::exception &e) {
    OBCX_DEBUG("获取QQ用户信息失败：{}", e.what());
  }
}

} // namespace bridge::qq

#include "qq/media_processor.hpp"

#include "config.hpp"
#include "media_processor.hpp"

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <iomanip>
#include <network/http_client.hpp>
#include <sstream>

namespace bridge::qq {

auto QQMediaProcessor::process_image_segment(
    obcx::core::IBot &qq_bot, obcx::core::IBot &telegram_bot,
    const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>> {

  obcx::common::MessageSegment converted_segment = segment;

  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  bool is_gif = false;
  if (!file_name.empty() && (file_name.find(".gif") != std::string::npos ||
                             file_name.find(".GIF") != std::string::npos)) {
    is_gif = true;
  }
  if (!url.empty() && url.find("gif") != std::string::npos) {
    is_gif = true;
  }

  // subType=1 在 QQ 上是动图表情：扩展名/URL 不一定带 gif，所以先查缓存，
  // 不命中再去下载文件头嗅探。失败时保守地按 GIF 处理。
  if (segment.data.contains("subType") && segment.data.at("subType") == 1 &&
      !url.empty()) {
    try {
      std::string qq_sticker_hash =
          storage::DatabaseManager::calculate_hash(url);
      auto cached_mapping =
          db_manager_->get_qq_sticker_mapping(qq_sticker_hash);

      if (cached_mapping && cached_mapping->is_gif.has_value()) {
        is_gif = cached_mapping->is_gif.value();
        PLUGIN_DEBUG("qq_to_tg", "使用缓存的图片类型检测结果: {} -> is_gif={}",
                     url, is_gif);
      } else {
        is_gif = co_await detect_gif_format(url);
      }
    } catch (const std::exception &e) {
      is_gif = true;
      PLUGIN_ERROR("qq_to_tg", "图片类型检测异常，回退到默认行为: {} - {}", url,
                   e.what());
    }
  }

  if (is_sticker(segment)) {
    bool handled = co_await handle_sticker_cache(
        telegram_bot, segment, telegram_group_id, topic_id, sender_display_name,
        bridge_config);
    if (handled) {
      co_return std::nullopt; // 已直接发送，不再加进普通消息
    }

    if (is_gif) {
      converted_segment.type = "animation";
    } else {
      converted_segment.type = "image";
    }
    PLUGIN_DEBUG("qq_to_tg", "检测到QQ表情包，使用压缩模式转发: {}", file_name);
  } else if (is_gif) {
    converted_segment.type = "animation";
    PLUGIN_DEBUG("qq_to_tg", "检测到QQ GIF动图，转为Telegram动画: {}",
                 file_name);
  } else {
    PLUGIN_DEBUG("qq_to_tg", "转发QQ图片文件: {}", file_name);
  }

  co_return converted_segment;
}

auto QQMediaProcessor::is_sticker(const obcx::common::MessageSegment &segment)
    -> bool {
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  if (!file_name.empty() && (file_name.find("sticker") != std::string::npos ||
                             file_name.find("emoji") != std::string::npos)) {
    return true;
  }
  // subType=1 在 QQ 表示 GIF 动图表情，也算表情包。
  if (segment.data.contains("subType") && segment.data.at("subType") == 1) {
    return true;
  }
  if (!url.empty() && (url.find("emoticon") != std::string::npos ||
                       url.find("sticker") != std::string::npos ||
                       url.find("emoji") != std::string::npos)) {
    return true;
  }

  return false;
}

auto QQMediaProcessor::handle_sticker_cache(
    obcx::core::IBot &telegram_bot, const obcx::common::MessageSegment &segment,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config) -> boost::asio::awaitable<bool> {

  try {
    std::string qq_sticker_hash = storage::DatabaseManager::calculate_hash(
        segment.data.value("file", "") + "_" + segment.data.value("url", ""));

    auto cached_mapping = db_manager_->get_qq_sticker_mapping(qq_sticker_hash);
    if (cached_mapping.has_value()) {
      db_manager_->update_qq_sticker_last_used(qq_sticker_hash);

      bool show_sender_for_sticker = false;
      if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
        show_sender_for_sticker = bridge_config->show_qq_to_tg_sender;
      } else {
        const TopicBridgeConfig *topic_config =
            get_topic_config(telegram_group_id, topic_id);
        show_sender_for_sticker =
            topic_config ? topic_config->show_qq_to_tg_sender : false;
      }

      std::string caption_info =
          show_sender_for_sticker ? fmt::format("[{}]\t", sender_display_name)
                                  : "";

      std::string response;
      if (topic_id == -1) {
        response = co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
                       .send_group_photo(telegram_group_id,
                                         cached_mapping->telegram_file_id,
                                         caption_info);
      } else {
        obcx::common::Message sticker_message;
        obcx::common::MessageSegment img_segment;
        img_segment.type = "image";
        img_segment.data["file"] = cached_mapping->telegram_file_id;
        if (!caption_info.empty()) {
          obcx::common::MessageSegment caption_segment;
          caption_segment.type = "text";
          caption_segment.data["text"] = caption_info;
          sticker_message.push_back(caption_segment);
        }
        sticker_message.push_back(img_segment);
        response = co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
                       .send_topic_message(telegram_group_id, topic_id,
                                           sticker_message);
      }

      PLUGIN_INFO("qq_to_tg", "使用缓存的QQ表情包发送成功: {} -> {}",
                  qq_sticker_hash, cached_mapping->telegram_file_id);
      co_return true;
    }
    PLUGIN_INFO("qq_to_tg", "QQ表情包缓存未命中，将上传并缓存: {}",
                qq_sticker_hash);
    co_return false;
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "处理QQ表情包缓存时出错: {}", e.what());
    co_return false;
  }
}

auto QQMediaProcessor::detect_gif_format(const std::string &url)
    -> boost::asio::awaitable<bool> {
  try {
    PLUGIN_INFO("qq_to_tg",
                "[图片类型检测] "
                "subType=1图片缓存未命中，开始下载文件进行本地检测: {}",
                url);

    const std::string &url_str(url);
    size_t protocol_pos = url_str.find("://");
    if (protocol_pos == std::string::npos) {
      throw std::runtime_error("无效的QQ文件URL格式");
    }

    size_t host_start = protocol_pos + 3;
    size_t path_start = url_str.find('/', host_start);
    if (path_start == std::string::npos) {
      throw std::runtime_error("QQ文件URL中未找到路径部分");
    }

    std::string host = url_str.substr(host_start, path_start - host_start);
    std::string path = url_str.substr(path_start);

    PLUGIN_DEBUG("qq_to_tg",
                 "[图片类型检测] QQ文件URL解析完成 - Host: {}, Path: {}", host,
                 path);

    // QQ 图片下载强制直连，不能走 Telegram 代理。
    obcx::common::ConnectionConfig qq_config;
    qq_config.host = host;
    qq_config.port = 443;
    qq_config.use_ssl = true;
    qq_config.access_token = "";
    qq_config.proxy_host = "";
    qq_config.proxy_port = 0;
    qq_config.proxy_type = "";
    qq_config.proxy_username = "";
    qq_config.proxy_password = "";

    PLUGIN_DEBUG("qq_to_tg",
                 "[图片类型检测] 创建专用QQ文件下载HttpClient - 主机: {}:{}",
                 host, qq_config.port);

    boost::asio::io_context temp_ioc;

    auto qq_http_client =
        std::make_unique<obcx::network::HttpClient>(temp_ioc, qq_config);

    // Range: 0-31 足以覆盖所有常见图片格式的 magic number。
    std::map<std::string, std::string> headers;
    headers["Range"] = "bytes=0-31";

    obcx::network::HttpResponse response =
        co_await qq_http_client->get(path, headers);

    if (response.is_success()) {
      std::string file_header = response.body;

      if (!file_header.empty()) {
        std::string detected_mime =
            MediaProcessor::detect_mime_type_from_content(file_header);
        bool is_gif = MediaProcessor::is_gif_from_content(file_header);

        PLUGIN_INFO("qq_to_tg",
                    "[图片类型检测] 文件头部MIME检测成功: {} -> {} "
                    "(is_gif={}, 读取了{}字节)",
                    url, detected_mime, is_gif, file_header.size());
        PLUGIN_DEBUG("qq_to_tg", "[图片类型检测] 文件头部16进制: {}",
                     to_hex_string(file_header));

        std::string qq_sticker_hash =
            storage::DatabaseManager::calculate_hash(url);
        storage::QQStickerMapping new_mapping;
        new_mapping.qq_sticker_hash = qq_sticker_hash;
        new_mapping.telegram_file_id = "";
        new_mapping.file_type = is_gif ? "animation" : "photo";
        new_mapping.is_gif = is_gif;
        new_mapping.content_type = detected_mime;
        new_mapping.created_at = std::chrono::system_clock::now();
        new_mapping.last_used_at = std::chrono::system_clock::now();
        new_mapping.last_checked_at = std::chrono::system_clock::now();
        db_manager_->save_qq_sticker_mapping(new_mapping);
        PLUGIN_DEBUG("qq_to_tg", "[图片类型检测] 缓存记录已保存");

        co_return is_gif;
      } else {
        PLUGIN_WARN("qq_to_tg",
                    "[图片类型检测] 文件头部内容为空，回退到默认行为: {}", url);
        co_return true;
      }
    } else {
      PLUGIN_WARN("qq_to_tg",
                  "[图片类型检测] Range请求失败，状态码: {}, "
                  "回退到默认行为: {}",
                  response.status_code, url);
      co_return true;
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg",
                 "[图片类型检测] "
                 "QQ文件Range请求或检测异常，回退到默认行为: {} - {}",
                 url, e.what());
    co_return true;
  }
}

auto QQMediaProcessor::to_hex_string(const std::string &data, size_t max_bytes)
    -> std::string {
  std::ostringstream oss;
  size_t len = std::min(data.size(), max_bytes);
  for (size_t i = 0; i < len; ++i) {
    if (i > 0)
      oss << " ";
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
        << static_cast<unsigned char>(data[i]);
  }
  if (data.size() > max_bytes) {
    oss << " ...";
  }
  return oss.str();
}

} // namespace bridge::qq

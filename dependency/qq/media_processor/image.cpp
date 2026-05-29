// QQ媒体处理器：图片 / 表情包 / GIF 相关逻辑。
//
// 包括：
//   - process_image_segment: 图片段总入口
//   - is_sticker: 表情包检测
//   - handle_sticker_cache: 通过 hash 缓存命中后直接发送
//   - detect_gif_format: 通过下载文件前若干字节本地检测GIF
//   - to_hex_string: 调试输出辅助

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

  // 检测是否为GIF图片或表情包
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  // 判断是否是GIF
  bool is_gif = false;
  if (!file_name.empty() && (file_name.find(".gif") != std::string::npos ||
                             file_name.find(".GIF") != std::string::npos)) {
    is_gif = true;
  }
  if (!url.empty() && url.find("gif") != std::string::npos) {
    is_gif = true;
  }

  // 对于subType=1的情况，使用数据库缓存和本地检测
  if (segment.data.contains("subType") && segment.data.at("subType") == 1 &&
      !url.empty()) {
    try {
      // 首先检查数据库缓存
      std::string qq_sticker_hash =
          storage::DatabaseManager::calculate_hash(url);
      auto cached_mapping =
          db_manager_->get_qq_sticker_mapping(qq_sticker_hash);

      if (cached_mapping && cached_mapping->is_gif.has_value()) {
        // 使用缓存的结果
        is_gif = cached_mapping->is_gif.value();
        PLUGIN_DEBUG("qq_to_tg", "使用缓存的图片类型检测结果: {} -> is_gif={}",
                     url, is_gif);
      } else {
        // 缓存未命中，进行本地检测
        is_gif = co_await detect_gif_format(url);
      }
    } catch (const std::exception &e) {
      // 异常情况下回退到旧逻辑
      is_gif = true;
      PLUGIN_ERROR("qq_to_tg", "图片类型检测异常，回退到默认行为: {} - {}", url,
                   e.what());
    }
  }

  // 检测是否为表情包
  if (is_sticker(segment)) {
    // QQ表情包处理：使用缓存系统优化
    bool handled = co_await handle_sticker_cache(
        telegram_bot, segment, telegram_group_id, topic_id, sender_display_name,
        bridge_config);
    if (handled) {
      co_return std::nullopt; // 已直接发送，不需要添加到普通消息中
    }

    // 缓存未命中或出错时，继续普通流程
    if (is_gif) {
      converted_segment.type = "animation";
    } else {
      converted_segment.type = "image"; // 使用photo而不是image以启用压缩
    }
    PLUGIN_DEBUG("qq_to_tg", "检测到QQ表情包，使用压缩模式转发: {}", file_name);
  } else if (is_gif) {
    // 普通GIF动图转换为Telegram animation
    converted_segment.type = "animation";
    PLUGIN_DEBUG("qq_to_tg", "检测到QQ GIF动图，转为Telegram动画: {}",
                 file_name);
  } else {
    // 普通图片保持不变
    PLUGIN_DEBUG("qq_to_tg", "转发QQ图片文件: {}", file_name);
  }

  co_return converted_segment;
}

auto QQMediaProcessor::is_sticker(const obcx::common::MessageSegment &segment)
    -> bool {
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  // 1. 检查文件名是否包含表情包特征
  if (!file_name.empty() && (file_name.find("sticker") != std::string::npos ||
                             file_name.find("emoji") != std::string::npos)) {
    return true;
  }
  // 2. 检查子类型 - subType=1可能表示动图表情
  if (segment.data.contains("subType") && segment.data.at("subType") == 1) {
    return true; // GIF表情包也算
  }
  // 3. 检查URL中的表情包特征
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
    // 计算QQ表情包的唯一hash
    std::string qq_sticker_hash = storage::DatabaseManager::calculate_hash(
        segment.data.value("file", "") + "_" + segment.data.value("url", ""));

    // 查询缓存
    auto cached_mapping = db_manager_->get_qq_sticker_mapping(qq_sticker_hash);
    if (cached_mapping.has_value()) {
      db_manager_->update_qq_sticker_last_used(qq_sticker_hash);

      // 根据模式获取显示发送者配置
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
        // 群组模式：发送到群组
        response = co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
                       .send_group_photo(telegram_group_id,
                                         cached_mapping->telegram_file_id,
                                         caption_info);
      } else {
        // Topic模式：使用topic消息发送
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
      co_return true; // 直接返回，不添加到普通消息中
    }
    // 缓存未命中，使用普通方式发送并保存file_id
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

    // 解析QQ文件URL获取主机和路径信息
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

    // 创建专用的HttpClient配置（直连，无代理）
    obcx::common::ConnectionConfig qq_config;
    qq_config.host = host;
    qq_config.port = 443; // HTTPS默认端口
    qq_config.use_ssl = true;
    qq_config.access_token = ""; // QQ文件下载不需要令牌
    // 确保直连，不使用代理
    qq_config.proxy_host = "";
    qq_config.proxy_port = 0;
    qq_config.proxy_type = "";
    qq_config.proxy_username = "";
    qq_config.proxy_password = "";

    PLUGIN_DEBUG("qq_to_tg",
                 "[图片类型检测] 创建专用QQ文件下载HttpClient - 主机: {}:{}",
                 host, qq_config.port);

    // 为QQ文件下载创建临时IO上下文
    boost::asio::io_context temp_ioc;

    // 创建专用的HttpClient实例（直连，无代理）
    auto qq_http_client =
        std::make_unique<obcx::network::HttpClient>(temp_ioc, qq_config);

    // 使用空的头部映射，让HttpClient设置完整的Firefox浏览器头部
    // 添加Range头部只请求前32个字节（足够检测所有常见图片格式的Magic Numbers）
    std::map<std::string, std::string> headers;
    headers["Range"] = "bytes=0-31";

    // 发送GET请求获取文件前32个字节
    obcx::network::HttpResponse response =
        co_await qq_http_client->get(path, headers);

    if (response.is_success()) {
      // 获取文件的前几个字节内容
      std::string file_header = response.body;

      if (!file_header.empty()) {
        // 使用文件头部Magic Numbers检测MIME类型
        std::string detected_mime =
            MediaProcessor::detect_mime_type_from_content(file_header);
        bool is_gif = MediaProcessor::is_gif_from_content(file_header);

        PLUGIN_INFO("qq_to_tg",
                    "[图片类型检测] 文件头部MIME检测成功: {} -> {} "
                    "(is_gif={}, 读取了{}字节)",
                    url, detected_mime, is_gif, file_header.size());
        PLUGIN_DEBUG("qq_to_tg", "[图片类型检测] 文件头部16进制: {}",
                     to_hex_string(file_header));

        // 创建新的缓存记录
        std::string qq_sticker_hash =
            storage::DatabaseManager::calculate_hash(url);
        storage::QQStickerMapping new_mapping;
        new_mapping.qq_sticker_hash = qq_sticker_hash;
        new_mapping.telegram_file_id = ""; // 暂时为空
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

#include "qq/media_processor.hpp"
#include "config.hpp"
#include "media_processor.hpp"
#include "qq/message_formatter.hpp"

#include <boost/asio/io_context.hpp>
#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <iomanip>
#include <network/http_client.hpp>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

namespace bridge::qq {

QQMediaProcessor::QQMediaProcessor(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto QQMediaProcessor::process_qq_media_segment(
    obcx::core::IBot &qq_bot, obcx::core::IBot &telegram_bot,
    const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>> {

  try {
    if (segment.type == "image") {
      co_return co_await process_image_segment(
          qq_bot, telegram_bot, segment, event, telegram_group_id, topic_id,
          sender_display_name, bridge_config, temp_files_to_cleanup);
    } else if (segment.type == "record") {
      co_return co_await process_record_segment(segment);
    } else if (segment.type == "video") {
      co_return co_await process_video_segment(segment);
    } else if (segment.type == "file") {
      co_return co_await process_file_segment(qq_bot, segment, event);
    } else if (segment.type == "face") {
      co_return co_await process_face_segment(segment);
    } else if (segment.type == "mface") {
      co_return co_await process_mface_segment(segment);
    } else if (segment.type == "at") {
      co_return co_await process_at_segment(
          qq_bot, segment, event, telegram_group_id, topic_id, bridge_config);
    } else if (segment.type == "shake") {
      co_return co_await process_shake_segment(segment);
    } else if (segment.type == "music") {
      co_return co_await process_music_segment(segment);
    } else if (segment.type == "share") {
      co_return co_await process_share_segment(segment);
    } else if (segment.type == "json") {
      co_return co_await process_json_segment(segment);
    } else if (segment.type == "app") {
      co_return co_await process_app_segment(segment);
    } else if (segment.type == "ark") {
      co_return co_await process_ark_segment(segment);
    } else if (segment.type == "miniapp") {
      co_return co_await process_miniapp_segment(segment);
    } else {
      // 保持原样
      co_return segment;
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "转换QQ消息段失败: {}", e.what());
    co_return std::nullopt;
  }
}

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

auto QQMediaProcessor::process_file_segment(
    obcx::core::IBot &qq_bot, const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment = segment;
  converted_segment.type = "document";

  // 提取QQ文件的file和url信息
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  PLUGIN_DEBUG("qq_to_tg", "转发QQ文件: file={}, url={}", file_name, url);

  // 提取更多信息用于诊断
  std::string file_id = segment.data.value("file_id", "");
  std::string file_size = segment.data.value("file_size", "");

  if (!url.empty()) {
    // 有URL时使用远程下载
    converted_segment.data["file"] = url;
    PLUGIN_DEBUG("qq_to_tg", "使用QQ文件URL进行转发: {}", url);
  } else if (!file_id.empty()) {
    // URL为空但有file_id时，使用LLOneBot的文件URL获取API
    PLUGIN_DEBUG("qq_to_tg", "URL为空，尝试通过file_id获取文件: {}", file_id);
    try {
      std::string response;
      // 根据消息来源选择API：群聊使用get_group_file_url，私聊使用get_private_file_url
      auto *qq_bot_ptr = static_cast<obcx::core::QQBot *>(&qq_bot);
      if (event.group_id.has_value()) {
        // 群聊文件
        std::string group_id = event.group_id.value();
        response = co_await qq_bot_ptr->get_group_file_url(group_id, file_id);
        PLUGIN_DEBUG("qq_to_tg", "get_group_file_url API响应: {}", response);
      } else {
        // 私聊文件
        std::string user_id = event.user_id;
        response = co_await qq_bot_ptr->get_private_file_url(user_id, file_id);
        PLUGIN_DEBUG("qq_to_tg", "get_private_file_url API响应: {}", response);
      }

      nlohmann::json response_json = nlohmann::json::parse(response);

      if (response_json.contains("status") && response_json["status"] == "ok" &&
          response_json.contains("data") &&
          response_json["data"].contains("url")) {
        std::string download_url = response_json["data"]["url"];
        converted_segment.data.erase("file_id");
        converted_segment.data["url"] = download_url;
        PLUGIN_DEBUG("qq_to_tg", "成功通过API获取文件下载URL: {}",
                     download_url);
      } else {
        throw std::runtime_error("API响应中没有找到下载URL");
      }
    } catch (const std::exception &e) {
      PLUGIN_WARN("qq_to_tg", "通过API获取文件URL失败: {}", e.what());
      // 转换为错误提示
      converted_segment.type = "text";
      converted_segment.data.clear();
      converted_segment.data["text"] = fmt::format(
          "[文件: {} ({} bytes)]\n❌ 无法获取下载链接", file_name, file_size);
    }
  } else {
    // 既没有URL也没有file_id
    converted_segment.type = "text";
    converted_segment.data.clear();
    converted_segment.data["text"] = fmt::format(
        "[文件: {} ({} bytes)]\n❌ 缺少文件信息", file_name, file_size);
    PLUGIN_WARN("qq_to_tg", "QQ文件缺少URL和file_id信息: {}", file_name);
  }

  co_return converted_segment;
}

auto QQMediaProcessor::process_at_segment(
    obcx::core::IBot &qq_bot, const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const GroupBridgeConfig *bridge_config)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment;
  converted_segment.type = "text";
  std::string qq_user_id = segment.data.value("qq", "unknown");
  converted_segment.data.clear();

  // 从数据库查询用户的显示名称（使用群组特定的昵称）
  auto at_display_name = db_manager_->query_user_display_name(
      "qq", qq_user_id, event.group_id.value_or(""));

  // 如果没有找到用户信息，尝试获取一次
  if (!at_display_name.has_value()) {
    // 尝试获取群成员信息并保存
    co_await QQMessageFormatter::fetch_and_save_user_info(
        db_manager_, qq_bot, qq_user_id, event.group_id.value());

    // 更新显示名称
    at_display_name = db_manager_->query_user_display_name(
        "qq", qq_user_id, event.group_id.value_or(""));
  }

  // 判断是否显示发送者信息（基于配置）
  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    // Topic模式：获取对应topic的配置
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  // 设置最终显示文本
  if (show_sender && at_display_name.has_value()) {
    converted_segment.data["text"] =
        fmt::format("@{} ", at_display_name.value());
    PLUGIN_DEBUG("qq_to_tg", "转换QQ@消息: {} -> @{}", qq_user_id,
                 at_display_name.value());
  } else if (show_sender) {
    converted_segment.data["text"] = fmt::format("[@{}] ", qq_user_id);
  } else {
    // 不显示发送者，返回空文本
    converted_segment.data["text"] = "";
    PLUGIN_DEBUG("qq_to_tg", "QQ@消息不显示发送者: {}", qq_user_id);
  }

  co_return converted_segment;
}

auto QQMediaProcessor::process_record_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted = segment;
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  PLUGIN_DEBUG("qq_to_tg", "转发QQ语音文件: file={}, url={}", file_name, url);

  // 优先使用URL进行远程下载
  if (!url.empty()) {
    converted.data["file"] = url;
  }

  co_return converted;
}

auto QQMediaProcessor::process_video_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted = segment;
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  PLUGIN_DEBUG("qq_to_tg", "转发QQ视频文件: file={}, url={}", file_name, url);

  // 优先使用URL进行远程下载
  if (!url.empty()) {
    converted.data["file"] = url;
  }

  co_return converted;
}

auto QQMediaProcessor::process_face_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string face_id = segment.data.value("id", "0");
  converted.data.clear();
  converted.data["text"] = fmt::format("[QQ表情:{}]", face_id);
  PLUGIN_DEBUG("qq_to_tg", "转换QQ表情为文本提示: face_id={}", face_id);
  co_return converted;
}

auto QQMediaProcessor::process_mface_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;

  // mface通常包含GIF表情包，提取URL和summary
  std::string url = segment.data.value("url", "");
  std::string summary = segment.data.value("summary", "");
  std::string emoji_id = segment.data.value("emoji_id", "");

  if (!url.empty()) {
    // 大部分QQ超级表情都是GIF格式，转换为Telegram的animation类型
    converted.type = "animation";
    converted.data["file"] = url;

    // 如果有summary，作为caption
    if (!summary.empty()) {
      converted.data["caption"] = summary;
    }

    PLUGIN_DEBUG("qq_to_tg",
                 "转换QQ超级表情为动画: url={}, summary={}, emoji_id={}", url,
                 summary, emoji_id);
  } else {
    // 如果没有URL，降级为文本提示
    converted.type = "text";
    converted.data.clear();
    if (!summary.empty()) {
      converted.data["text"] = fmt::format("[表情包:{}]", summary);
    } else {
      converted.data["text"] = "[QQ超级表情]";
    }
    PLUGIN_WARN("qq_to_tg", "QQ超级表情缺少URL，转换为文本: summary={}",
                summary);
  }

  co_return converted;
}

auto QQMediaProcessor::process_shake_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  converted.data.clear();
  converted.data["text"] = "[戳一戳]";
  PLUGIN_DEBUG("qq_to_tg", "转换QQ戳一戳为文本提示");
  co_return converted;
}

auto QQMediaProcessor::process_music_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string title = segment.data.value("title", "未知音乐");
  converted.data.clear();
  converted.data["text"] = fmt::format("[音乐分享: {}]", title);
  PLUGIN_DEBUG("qq_to_tg", "转换QQ音乐分享为文本: title={}", title);
  co_return converted;
}

auto QQMediaProcessor::process_share_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string url = segment.data.value("url", "");
  std::string title = segment.data.value("title", "链接分享");
  converted.data.clear();
  converted.data["text"] = fmt::format("[{}]\t{}", title, url);
  PLUGIN_DEBUG("qq_to_tg", "转换QQ链接分享为文本: title={}, url={}", title,
               url);
  co_return converted;
}

auto QQMediaProcessor::process_json_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";

  try {
    std::string json_data = segment.data.value("data", "");
    if (!json_data.empty()) {
      auto parse_result = parse_miniapp_json(json_data);
      converted = format_miniapp_message(parse_result);
      PLUGIN_DEBUG("qq_to_tg", "转换QQ小程序JSON: success={}, title={}",
                   parse_result.success, parse_result.title);
    } else {
      converted.data.clear();
      converted.data["text"] = "📱 [小程序-无数据]";
      PLUGIN_DEBUG("qq_to_tg", "QQ小程序JSON消息无数据");
    }
  } catch (const std::exception &e) {
    converted.data.clear();
    converted.data["text"] = "📱 [小程序解析错误]";
    PLUGIN_ERROR("qq_to_tg", "处理QQ小程序JSON时出错: {}", e.what());
  }

  co_return converted;
}

auto QQMediaProcessor::process_app_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";

  try {
    std::string app_data = segment.data.dump();
    auto parse_result = parse_miniapp_json(app_data);
    if (!parse_result.success) {
      // 如果JSON解析失败，尝试直接提取字段
      parse_result.title = segment.data.value("title", "应用分享");
      parse_result.description = segment.data.value("content", "");
      parse_result.app_name = segment.data.value("name", "");
      if (segment.data.contains("url")) {
        parse_result.urls.push_back(segment.data.value("url", ""));
        parse_result.success = true;
      }
    }
    converted = format_miniapp_message(parse_result);
    PLUGIN_DEBUG("qq_to_tg", "转换QQ应用分享: success={}, title={}",
                 parse_result.success, parse_result.title);
  } catch (const std::exception &e) {
    converted.data.clear();
    converted.data["text"] = "📱 [应用分享解析错误]";
    PLUGIN_ERROR("qq_to_tg", "处理QQ应用分享时出错: {}", e.what());
  }

  co_return converted;
}

auto QQMediaProcessor::process_ark_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";

  try {
    std::string ark_data = segment.data.dump();
    auto parse_result = parse_miniapp_json(ark_data);
    if (!parse_result.success) {
      // ARK消息的特殊处理
      parse_result.title = segment.data.value("prompt", "ARK卡片");
      parse_result.description = segment.data.value("desc", "");

      // 从kv数组中提取信息
      if (segment.data.contains("kv") && segment.data.at("kv").is_array()) {
        for (const auto &kv : segment.data.at("kv")) {
          if (kv.contains("key") && kv.contains("value")) {
            std::string key = kv["key"];
            if (key.find("URL") != std::string::npos ||
                key.find("url") != std::string::npos) {
              parse_result.urls.push_back(kv["value"]);
            }
          }
        }
      }
      parse_result.success =
          !parse_result.urls.empty() || !parse_result.title.empty();
    }
    converted = format_miniapp_message(parse_result);
    PLUGIN_DEBUG("qq_to_tg", "转换QQ ARK卡片: success={}, title={}",
                 parse_result.success, parse_result.title);
  } catch (const std::exception &e) {
    converted.data.clear();
    converted.data["text"] = "📱 [ARK卡片解析错误]";
    PLUGIN_ERROR("qq_to_tg", "处理QQ ARK卡片时出错: {}", e.what());
  }

  co_return converted;
}

auto QQMediaProcessor::process_miniapp_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";

  try {
    std::string miniapp_data = segment.data.dump();
    auto parse_result = parse_miniapp_json(miniapp_data);
    if (!parse_result.success) {
      // 小程序消息的直接字段提取
      parse_result.title = segment.data.value("title", "小程序");
      parse_result.description = segment.data.value("desc", "");
      parse_result.app_name = segment.data.value("appid", "");
      if (segment.data.contains("url")) {
        parse_result.urls.push_back(segment.data.value("url", ""));
        parse_result.success = true;
      }
    }
    converted = format_miniapp_message(parse_result);
    PLUGIN_DEBUG("qq_to_tg", "转换QQ小程序: success={}, title={}",
                 parse_result.success, parse_result.title);
  } catch (const std::exception &e) {
    converted.data.clear();
    converted.data["text"] = "📱 [小程序解析错误]";
    PLUGIN_ERROR("qq_to_tg", "处理QQ小程序时出错: {}", e.what());
  }

  co_return converted;
}

auto QQMediaProcessor::detect_gif_format(const std::string &url)
    -> boost::asio::awaitable<bool> {
  try {
    PLUGIN_INFO("qq_to_tg",
                "[图片类型检测] "
                "subType=1图片缓存未命中，开始下载文件进行本地检测: {}",
                url);

    // 解析QQ文件URL获取主机和路径信息
    std::string url_str(url);
    size_t protocol_pos = url_str.find("://");
    if (protocol_pos == std::string::npos) {
      throw std::runtime_error("无效的QQ文件URL格式");
    }

    size_t host_start = protocol_pos + 3;
    size_t path_start = url_str.find("/", host_start);
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
        response = co_await static_cast<obcx::core::TGBot &>(telegram_bot)
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
        response = co_await static_cast<obcx::core::TGBot &>(telegram_bot)
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

auto QQMediaProcessor::parse_miniapp_json(const std::string &json_data)
    -> MiniAppParseResult {
  MiniAppParseResult result;
  result.raw_json = json_data;

  if (!config::ENABLE_MINIAPP_PARSING) {
    return result;
  }

  try {
    nlohmann::json j = nlohmann::json::parse(json_data);

    // 提取应用名称
    if (j.contains("app")) {
      result.app_name = j["app"];
    }

    // 提取标题
    if (j.contains("prompt")) {
      result.title = j["prompt"];
    } else if (j.contains("meta") && j["meta"].contains("detail") &&
               j["meta"]["detail"].contains("title")) {
      result.title = j["meta"]["detail"]["title"];
    }

    // 提取描述
    if (j.contains("desc")) {
      result.description = j["desc"];
    } else if (j.contains("meta") && j["meta"].contains("detail") &&
               j["meta"]["detail"].contains("desc")) {
      result.description = j["meta"]["detail"]["desc"];
    }

    // 提取URLs - 多个位置查找
    std::vector<std::string> found_urls;

    // 1. 从meta.url提取
    if (j.contains("meta")) {
      auto meta = j["meta"];
      if (meta.contains("url") && meta["url"].is_string()) {
        found_urls.push_back(meta["url"]);
      }
      if (meta.contains("detail")) {
        auto detail = meta["detail"];
        if (detail.contains("url") && detail["url"].is_string()) {
          found_urls.push_back(detail["url"]);
        }
      }
    }

    // 2. 从顶级字段提取
    if (j.contains("url") && j["url"].is_string()) {
      found_urls.push_back(j["url"]);
    }

    // 3. 从任何地方用正则表达式提取
    auto regex_urls = extract_urls_from_json(json_data);
    found_urls.insert(found_urls.end(), regex_urls.begin(), regex_urls.end());

    // 去重
    std::sort(found_urls.begin(), found_urls.end());
    found_urls.erase(std::unique(found_urls.begin(), found_urls.end()),
                     found_urls.end());

    result.urls = found_urls;
    result.success = !found_urls.empty() || !result.title.empty();

    PLUGIN_DEBUG("qq_to_tg", "解析小程序: app={}, title={}, urls_count={}",
                 result.app_name, result.title, result.urls.size());

  } catch (const std::exception &e) {
    PLUGIN_DEBUG("qq_to_tg", "小程序JSON解析失败: {}", e.what());
    // 解析失败时仍然尝试用正则提取URL
    result.urls = extract_urls_from_json(json_data);
    result.success = !result.urls.empty();
  }

  return result;
}

auto QQMediaProcessor::format_miniapp_message(
    const MiniAppParseResult &parse_result) -> obcx::common::MessageSegment {
  obcx::common::MessageSegment segment;
  segment.type = "text";

  std::string message_text;

  if (parse_result.success) {
    // 成功解析的情况
    message_text = "📱 ";

    if (!parse_result.title.empty()) {
      message_text += fmt::format("[{}]", parse_result.title);
    } else {
      message_text += "[小程序]";
    }

    if (!parse_result.description.empty() &&
        parse_result.description != parse_result.title) {
      message_text += fmt::format("\n{}", parse_result.description);
    }

    if (!parse_result.urls.empty()) {
      message_text += "\n🔗 链接:";
      for (const auto &url : parse_result.urls) {
        message_text += fmt::format("\n{}", url);
      }
    }

    if (!parse_result.app_name.empty()) {
      message_text += fmt::format("\n📦 应用: {}", parse_result.app_name);
    }

  } else {
    // 解析失败的情况
    message_text = "📱 [无法解析的小程序]";

    if (config::SHOW_RAW_JSON_ON_PARSE_FAIL) {
      std::string json_to_show = parse_result.raw_json;
      if (json_to_show.length() > config::MAX_JSON_DISPLAY_LENGTH) {
        json_to_show =
            json_to_show.substr(0, config::MAX_JSON_DISPLAY_LENGTH) + "...";
      }
      message_text +=
          fmt::format("\n原始数据:\n```json\n{}\n```", json_to_show);
    }
  }

  segment.data["text"] = message_text;
  return segment;
}

auto QQMediaProcessor::extract_urls_from_json(const std::string &json_str)
    -> std::vector<std::string> {
  std::vector<std::string> urls;

  // 使用正则表达式匹配JSON中的URL
  std::regex url_regex(R"((https?://[^\s\",}]+))");
  std::sregex_iterator url_iter(json_str.begin(), json_str.end(), url_regex);
  std::sregex_iterator url_end;

  for (; url_iter != url_end; ++url_iter) {
    urls.push_back(url_iter->str());
  }

  return urls;
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

#include "telegram/media_processor.hpp"
#include "config.hpp"
#include "media_processor.hpp"

#include "path_manager.hpp"
#include <common/logger.hpp>
#include <common/media_converter.hpp>
#include <core/tg_bot.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <telegram/network/connection_manager.hpp>
#include <utility>

namespace bridge::telegram {

TelegramMediaProcessor::TelegramMediaProcessor(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto TelegramMediaProcessor::process_media_file(
    obcx::core::IBot &telegram_bot, const std::string &file_type,
    const std::string &file_id, const nlohmann::json &media_data,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::vector<obcx::common::MessageSegment>> {

  std::vector<obcx::common::MessageSegment> result;

  try {
    if (file_id.empty()) {
      // 发送文件类型提示
      obcx::common::MessageSegment text_segment;
      text_segment.type = "text";
      std::string type_name = file_type == "photo" || file_type == "image"
                                  ? "图片"
                              : file_type == "video"      ? "视频"
                              : file_type == "audio"      ? "音频"
                              : file_type == "voice"      ? "语音"
                              : file_type == "sticker"    ? "贴纸"
                              : file_type == "animation"  ? "GIF动画"
                              : file_type == "video_note" ? "视频消息"
                              : file_type == "document"   ? "文档"
                                                          : "文件";
      text_segment.data["text"] = fmt::format("[{}]", type_name);
      result.push_back(text_segment);
      co_return result;
    }

    // 使用TGBot的新接口获取文件URL
    obcx::core::MediaFileInfo media_info;
    media_info.file_id = file_id;
    media_info.file_type = file_type;

    auto download_url_opt =
        co_await static_cast<obcx::core::TGBot &>(telegram_bot)
            .get_media_download_url(media_info);
    if (!download_url_opt.has_value()) {
      throw std::runtime_error("无法获取文件下载链接");
    }

    std::string file_url = download_url_opt.value();
    auto [final_url, filename] =
        MediaProcessor::get_qq_file_info(file_url, file_type);

    obcx::common::MessageSegment file_segment;

    // 根据文件类型创建相应的消息段
    if (file_type == "photo" || file_type == "image") {
      file_segment = co_await process_photo(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "video") {
      file_segment = co_await process_video(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "audio" || file_type == "voice") {
      file_segment = co_await process_audio(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "document") {
      file_segment = co_await process_document(telegram_bot, file_url, filename,
                                               temp_files_to_cleanup);
    } else if (file_type == "sticker") {
      // 补充MediaFileInfo中缺少的信息
      if (media_data.contains("sticker")) {
        auto sticker = media_data["sticker"];
        if (sticker.contains("file_size")) {
          media_info.file_size = sticker["file_size"].get<int64_t>();
        }
        if (sticker.contains("file_unique_id")) {
          media_info.file_unique_id =
              sticker["file_unique_id"].get<std::string>();
        }
        if (sticker.contains("is_animated") &&
            sticker["is_animated"].get<bool>()) {
          media_info.mime_type = "application/tgs";
        } else if (sticker.contains("is_video") &&
                   sticker["is_video"].get<bool>()) {
          media_info.mime_type = "video/webm";
        } else {
          media_info.mime_type = "image/webp";
        }
      }
      file_segment =
          co_await process_sticker(telegram_bot, media_info, media_data);
    } else if (file_type == "animation") {
      // 补充MediaFileInfo中缺少的file_unique_id信息
      if (media_data.contains("animation")) {
        auto animation = media_data["animation"];
        if (animation.contains("file_unique_id")) {
          media_info.file_unique_id =
              animation["file_unique_id"].get<std::string>();
        }
      }
      file_segment = co_await process_animation(telegram_bot, media_info,
                                                media_data, filename);
    } else if (file_type == "video_note") {
      file_segment = co_await process_video_note(
          telegram_bot, file_url, filename, temp_files_to_cleanup);
    } else {
      file_segment = co_await process_other_file(
          telegram_bot, file_url, filename, temp_files_to_cleanup);
    }

    result.push_back(file_segment);
    // convert caption to text
    if (media_data.contains("caption") &&
        !media_data["caption"].get<std::string>().empty()) {
      nlohmann::json caption_text;
      caption_text["type"] = "text";
      caption_text["text"] = media_data["caption"].get<std::string>();
      result.push_back(obcx::common::MessageSegment{
          .type = "text", .data = std::move(caption_text)});
    }
    PLUGIN_INFO("tg_to_qq", "成功处理Telegram {}文件: {}", file_type, filename);

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理媒体文件失败: {}", e.what());
    // obcx::common::MessageSegment error_segment;
    // error_segment.type = "text";
    // error_segment.data["text"] = fmt::format("[{}处理失败]", file_type);
    // result.push_back(error_segment);
  }

  co_return result;
}

auto TelegramMediaProcessor::process_photo(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "photo", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "image";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["proxy"] = 1;
      PLUGIN_INFO("tg_to_qq", "成功下载图片到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载图片失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载图片失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "photo");
    file_segment.type = "image";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_video(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "video", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载视频到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载视频失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载视频失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "video");
    file_segment.type = "video";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_audio(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "audio", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "record";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载音频到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载音频失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载音频失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "audio");
    file_segment.type = "record";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_document(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "document", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      PLUGIN_INFO("tg_to_qq", "成功下载文档到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载文档失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载文档失败，回退到URL方式: {}", e.what());
    auto [final_url, _] =
        MediaProcessor::get_qq_file_info(file_url, "document");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_sticker(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const nlohmann::json &media_data)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto cached_path_opt = co_await download_sticker_with_cache(
        telegram_bot, media_info, "/tmp/bridge_files");

    if (cached_path_opt.has_value()) {
      std::string container_file_path = cached_path_opt.value();

      file_segment.type = "image";
      file_segment.data.clear();
      file_segment.data["file"] = container_file_path;

      // 添加贴纸信息
      if (media_data.contains("sticker")) {
        auto sticker = media_data["sticker"];
        std::string sticker_info = "[贴纸";
        if (sticker.contains("emoji")) {
          sticker_info += " " + sticker["emoji"].get<std::string>();
        }
        if (sticker.contains("is_animated") &&
            sticker["is_animated"].get<bool>()) {
          sticker_info += " 动画";
        } else if (sticker.contains("is_video") &&
                   sticker["is_video"].get<bool>()) {
          sticker_info += " 视频";
        }
        sticker_info += "]";
        file_segment.data["caption"] = sticker_info;
      }

      PLUGIN_INFO("tg_to_qq", "成功缓存Telegram sticker到容器路径: {}",
                  container_file_path);
    } else {
      throw std::runtime_error("缓存下载失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "缓存系统处理表情包失败: {}, 回退为文本提示",
                e.what());

    file_segment.type = "text";
    std::string emoji_info = "";
    if (media_data.contains("sticker") &&
        media_data["sticker"].contains("emoji")) {
      emoji_info = " " + media_data["sticker"]["emoji"].get<std::string>();
    }
    file_segment.data["text"] = fmt::format("[贴纸{}]", emoji_info);
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_animation(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const nlohmann::json &media_data, const std::string &filename)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto cached_path_opt = co_await download_animation_with_cache(
        telegram_bot, media_info, "/tmp/bridge_files");

    if (cached_path_opt.has_value()) {
      std::string container_file_path = cached_path_opt.value();

      file_segment.type = "file";
      file_segment.data.clear();
      file_segment.data["file"] = "file:///" + container_file_path;
      file_segment.data["name"] = filename;

      PLUGIN_INFO("tg_to_qq", "成功缓存Telegram animation到容器路径: {}",
                  container_file_path);
    } else {
      throw std::runtime_error("缓存下载失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "缓存系统处理动画失败: {}, 回退到URL方式",
                e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info("", "animation");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_video_note(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "video_note", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载视频消息到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载视频消息失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载视频消息失败，回退到URL方式: {}", e.what());
    auto [final_url, _] =
        MediaProcessor::get_qq_file_info(file_url, "video_note");
    file_segment.type = "video";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_other_file(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &static_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "file", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      PLUGIN_INFO("tg_to_qq", "成功下载其他类型文件到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载其他类型文件失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载其他类型文件失败，回退到URL方式: {}",
                e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "file");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::download_sticker_with_cache(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const std::string &bridge_files_dir)
    -> boost::asio::awaitable<std::optional<std::string>> {

  try {
    // 检查是否是表情包类型
    if (media_info.file_type != "sticker") {
      PLUGIN_ERROR("tg_to_qq", "不支持的文件类型，仅支持sticker: {}",
                   media_info.file_type);
      co_return std::nullopt;
    }

    // 严格使用file_unique_id作为唯一键，不使用任何hash
    if (media_info.file_unique_id.empty()) {
      PLUGIN_WARN("tg_to_qq",
                  "file_unique_id为空，跳过数据库缓存操作，直接下载: {}",
                  media_info.file_id);
      // 不使用缓存，直接下载
    } else {
      std::string cache_key = media_info.file_unique_id;
      PLUGIN_DEBUG("tg_to_qq", "表情包缓存查找，使用file_unique_id: {}",
                   cache_key);

      // 查询缓存
      auto cache_info = db_manager_->get_sticker_cache("telegram", cache_key);
      if (cache_info.has_value()) {
        // 缓存命中，但需要验证文件是否真实存在
        bool file_exists = false;

        // 检查最终使用的文件是否存在
        if (cache_info->conversion_status == "success" &&
            cache_info->converted_file_path.has_value()) {
          // 优先使用转换后的文件
          std::string host_path = cache_info->converted_file_path.value();
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        } else if (!cache_info->original_file_path.empty()) {
          // 使用原始文件
          std::string host_path = cache_info->original_file_path;
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        }

        if (file_exists && !cache_info->container_path.empty()) {
          // 更新最后使用时间
          storage::StickerCacheInfo update_info = *cache_info;
          update_info.last_used_at = std::chrono::system_clock::now();
          db_manager_->save_sticker_cache(update_info);

          PLUGIN_DEBUG("tg_to_qq", "表情包缓存命中: {} -> {}", cache_key,
                       cache_info->container_path);
          co_return cache_info->container_path;
        } else {
          PLUGIN_WARN("tg_to_qq", "表情包缓存项存在但文件丢失，将重新下载: {}",
                      cache_key);
        }
      }
    }

    // 缓存未命中或文件不存在，需要下载
    PLUGIN_INFO("tg_to_qq", "表情包缓存未命中，开始下载: {}",
                media_info.file_id);

    // 获取下载URL
    auto download_urls = co_await static_cast<obcx::core::TGBot &>(telegram_bot)
                             .get_media_download_urls({media_info});
    if (download_urls.empty() || !download_urls[0].has_value()) {
      PLUGIN_ERROR("tg_to_qq", "获取表情包下载URL失败: {}", media_info.file_id);
      co_return std::nullopt;
    }

    std::string download_url = download_urls[0].value();

    // 使用配置中的挂载点路径
    std::string host_bridge_files_dir = bridge::config::BRIDGE_FILES_DIR;
    std::string original_dir = host_bridge_files_dir + "/stickers/original";
    std::filesystem::create_directories(original_dir);

    // 检测文件类型和扩展名
    std::string file_extension = ".webp"; // 默认webp
    std::string mime_type = "image/webp";

    if (media_info.mime_type.has_value()) {
      mime_type = media_info.mime_type.value();
      if (mime_type == "image/webp") {
        file_extension = ".webp";
      } else if (mime_type == "video/webm") {
        file_extension = ".webm";
      } else if (mime_type == "application/tgs") {
        file_extension = ".tgs";
      }
    }

    // 生成原始文件路径
    std::string filename_prefix;
    if (!media_info.file_unique_id.empty()) {
      // 使用 file_unique_id 作为文件名前缀
      filename_prefix =
          fmt::format("sticker_{}_{}", media_info.file_unique_id.substr(0, 12),
                      media_info.file_id.substr(0, 8));
    } else {
      // 没有 file_unique_id，使用时间戳
      auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      filename_prefix = fmt::format("sticker_{}_{}", timestamp,
                                    media_info.file_id.substr(0, 8));
    }
    std::string original_filename = filename_prefix + file_extension;
    std::string original_file_path = original_dir + "/" + original_filename;

    // 下载文件内容
    auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&telegram_bot);
    if (!tg_bot) {
      PLUGIN_ERROR("tg_to_qq", "telegram_bot不是TGBot类型");
      co_return std::nullopt;
    }

    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      PLUGIN_ERROR("tg_to_qq", "连接管理器不是TelegramConnectionManager类型");
      co_return std::nullopt;
    }

    auto file_content =
        co_await conn_manager->download_file_content(download_url);
    if (file_content.empty()) {
      PLUGIN_ERROR("tg_to_qq", "下载文件内容为空: {}", download_url);
      co_return std::nullopt;
    }

    // 保存原始文件
    std::ofstream file(original_file_path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("无法创建文件: " + original_file_path);
    }
    file.write(file_content.data(), file_content.size());
    file.close();

    PLUGIN_INFO("tg_to_qq", "表情包原始文件已下载: {} -> {} ({}字节)",
                media_info.file_id, original_file_path, file_content.size());

    std::string final_file_path = original_file_path;
    std::string final_container_path;
    std::string conversion_status = "success";

    // 如果是webm格式，需要转换为gif
    if (mime_type == "video/webm" || file_extension == ".webm") {
      PLUGIN_INFO("tg_to_qq", "检测到webm格式贴纸，开始转换为gif: {}",
                  original_file_path);

      // 创建转换目录
      std::string converted_dir = host_bridge_files_dir + "/stickers/converted";
      std::filesystem::create_directories(converted_dir);

      // 生成转换后的gif文件路径
      std::string converted_filename = filename_prefix + ".gif";
      std::string converted_file_path =
          converted_dir + "/" + converted_filename;

      // 使用MediaConverter进行转换
      bool conversion_success =
          obcx::common::MediaConverter::convert_webm_to_gif_with_fallback(
              original_file_path, converted_file_path, 5);

      if (conversion_success && std::filesystem::exists(converted_file_path)) {
        PLUGIN_INFO("tg_to_qq", "webm贴纸到gif转换成功: {} -> {}",
                    original_file_path, converted_file_path);
        final_file_path = converted_file_path;
        final_container_path =
            "/root/llonebot/bridge_files/stickers/converted/" +
            converted_filename;
        conversion_status = "success";
      } else {
        PLUGIN_WARN("tg_to_qq", "webm贴纸到gif转换失败，使用原始webm文件: {}",
                    original_file_path);
        final_container_path =
            "/root/llonebot/bridge_files/stickers/original/" +
            original_filename;
        conversion_status = "failed";
      }
    } else {
      // 非webm格式，直接使用原始文件
      final_container_path =
          "/root/llonebot/bridge_files/stickers/original/" + original_filename;
    }

    // 只有在有 file_unique_id 时才保存到数据库
    if (!media_info.file_unique_id.empty()) {
      // 创建缓存信息
      storage::StickerCacheInfo new_cache_info;
      new_cache_info.platform = "telegram";
      new_cache_info.sticker_id = media_info.file_id; // 原始file_id
      new_cache_info.sticker_hash =
          media_info.file_unique_id; // 用于查询的唯一ID
      new_cache_info.original_file_path = original_file_path; // 主机路径
      new_cache_info.file_size = file_content.size();
      new_cache_info.mime_type = mime_type;
      new_cache_info.conversion_status = conversion_status;
      new_cache_info.created_at = std::chrono::system_clock::now();
      new_cache_info.last_used_at = std::chrono::system_clock::now();
      new_cache_info.container_path = final_container_path; // 容器内路径

      // 如果有转换后的文件，也保存转换后的路径
      if (final_file_path != original_file_path) {
        new_cache_info.converted_file_path = final_file_path;
      }

      // 保存到缓存数据库
      if (!db_manager_->save_sticker_cache(new_cache_info)) {
        PLUGIN_WARN("tg_to_qq", "保存表情包缓存失败，但文件已下载: {}",
                    final_file_path);
      }
    } else {
      PLUGIN_DEBUG("tg_to_qq", "没有file_unique_id，跳过数据库保存");
    }

    PLUGIN_INFO("tg_to_qq", "表情包缓存完成: {} -> {}", media_info.file_id,
                final_container_path);
    co_return final_container_path;

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "下载表情包失败 (file_id: {}): {}",
                 media_info.file_id, e.what());
    co_return std::nullopt;
  }
}

auto TelegramMediaProcessor::download_animation_with_cache(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const std::string &bridge_files_dir)
    -> boost::asio::awaitable<std::optional<std::string>> {

  try {
    // 检查是否是动画类型
    if (media_info.file_type != "animation") {
      PLUGIN_ERROR("tg_to_qq", "不支持的文件类型，仅支持animation: {}",
                   media_info.file_type);
      co_return std::nullopt;
    }

    // 严格使用file_unique_id作为唯一键，不使用任何hash
    if (media_info.file_unique_id.empty()) {
      PLUGIN_WARN("tg_to_qq",
                  "file_unique_id为空，跳过数据库缓存操作，直接下载: {}",
                  media_info.file_id);
      // 不使用缓存，直接下载
    } else {
      std::string cache_key = media_info.file_unique_id;
      PLUGIN_DEBUG("tg_to_qq", "动画缓存查找，使用file_unique_id: {}",
                   cache_key);

      // 查询缓存 - 使用专门的animation缓存表
      auto cache_info =
          db_manager_->get_sticker_cache("telegram_animation", cache_key);
      if (cache_info.has_value()) {
        // 缓存命中，但需要验证文件是否真实存在
        bool file_exists = false;

        // 检查最终使用的文件是否存在
        if (cache_info->conversion_status == "success" &&
            cache_info->converted_file_path.has_value()) {
          // 优先使用转换后的gif文件
          std::string host_path = cache_info->converted_file_path.value();
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        } else if (!cache_info->original_file_path.empty()) {
          // 使用原始文件
          std::string host_path = cache_info->original_file_path;
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        }

        if (file_exists && !cache_info->container_path.empty()) {
          // 更新最后使用时间
          storage::StickerCacheInfo update_info = *cache_info;
          update_info.last_used_at = std::chrono::system_clock::now();
          db_manager_->save_sticker_cache(update_info);

          PLUGIN_DEBUG("tg_to_qq", "动画缓存命中: {} -> {}", cache_key,
                       cache_info->container_path);
          co_return cache_info->container_path;
        } else {
          PLUGIN_WARN("tg_to_qq", "动画缓存项存在但文件丢失，将重新下载: {}",
                      cache_key);
        }
      }
    }

    // 缓存未命中或文件不存在，需要下载
    PLUGIN_INFO("tg_to_qq", "动画缓存未命中，开始下载: {}", media_info.file_id);

    // 获取下载URL
    auto download_urls = co_await static_cast<obcx::core::TGBot &>(telegram_bot)
                             .get_media_download_urls({media_info});
    if (download_urls.empty() || !download_urls[0].has_value()) {
      PLUGIN_ERROR("tg_to_qq", "获取动画下载URL失败: {}", media_info.file_id);
      co_return std::nullopt;
    }

    std::string download_url = download_urls[0].value();

    // 使用配置中的挂载点路径
    std::string host_bridge_files_dir = bridge::config::BRIDGE_FILES_DIR;
    std::string original_dir = host_bridge_files_dir + "/animations/original";
    std::string converted_dir = host_bridge_files_dir + "/animations/converted";
    std::filesystem::create_directories(original_dir);
    std::filesystem::create_directories(converted_dir);

    // 检测文件类型和扩展名
    std::string file_extension = ".mp4"; // 默认mp4
    std::string mime_type = "video/mp4";

    if (media_info.mime_type.has_value()) {
      mime_type = media_info.mime_type.value();
      if (mime_type == "video/mp4") {
        file_extension = ".mp4";
      } else if (mime_type == "video/webm") {
        file_extension = ".webm";
      } else if (mime_type == "image/gif") {
        file_extension = ".gif";
      }
    }

    // 生成原始文件路径
    std::string filename_prefix;
    if (!media_info.file_unique_id.empty()) {
      // 使用 file_unique_id 作为文件名前缀
      filename_prefix = fmt::format("animation_{}_{}",
                                    media_info.file_unique_id.substr(0, 12),
                                    media_info.file_id.substr(0, 8));
    } else {
      // 没有 file_unique_id，使用时间戳
      auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      filename_prefix = fmt::format("animation_{}_{}", timestamp,
                                    media_info.file_id.substr(0, 8));
    }
    std::string original_filename = filename_prefix + file_extension;
    std::string original_file_path = original_dir + "/" + original_filename;

    // 下载文件内容
    auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&telegram_bot);
    if (!tg_bot) {
      PLUGIN_ERROR("tg_to_qq", "telegram_bot不是TGBot类型");
      co_return std::nullopt;
    }

    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      PLUGIN_ERROR("tg_to_qq", "连接管理器不是TelegramConnectionManager类型");
      co_return std::nullopt;
    }

    auto file_content =
        co_await conn_manager->download_file_content(download_url);
    if (file_content.empty()) {
      PLUGIN_ERROR("tg_to_qq", "下载文件内容为空: {}", download_url);
      co_return std::nullopt;
    }

    // 保存原始文件
    std::ofstream file(original_file_path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("无法创建文件: " + original_file_path);
    }
    file.write(file_content.data(), file_content.size());
    file.close();

    PLUGIN_INFO("tg_to_qq", "动画原始文件已下载: {} -> {} ({}字节)",
                media_info.file_id, original_file_path, file_content.size());

    std::string final_file_path = original_file_path;
    std::string final_container_path;
    std::string conversion_status = "success";

    // 如果是webm格式，需要转换为gif
    if (mime_type == "video/webm" || file_extension == ".webm") {
      PLUGIN_INFO("tg_to_qq", "检测到webm格式动画，开始转换为gif: {}",
                  original_file_path);

      // 生成转换后的gif文件路径
      std::string converted_filename = filename_prefix + ".gif";
      std::string converted_file_path =
          converted_dir + "/" + converted_filename;
      if (!std::filesystem::exists(converted_dir)) {
        std::filesystem::create_directories(converted_dir);
      }

      // 使用MediaConverter进行转换
      bool conversion_success =
          obcx::common::MediaConverter::convert_webm_to_gif_with_fallback(
              original_file_path, converted_file_path, 5);

      if (conversion_success && std::filesystem::exists(converted_file_path)) {
        PLUGIN_INFO("tg_to_qq", "webm到gif转换成功: {} -> {}",
                    original_file_path, converted_file_path);
        final_file_path = converted_file_path;
        final_container_path =
            "/root/llonebot/bridge_files/animations/converted/" +
            converted_filename;
        conversion_status = "success";
      } else {
        PLUGIN_WARN("tg_to_qq", "webm到gif转换失败，使用原始webm文件: {}",
                    original_file_path);
        final_container_path =
            "/root/llonebot/bridge_files/animations/original/" +
            original_filename;
        conversion_status = "failed";
      }
    } else {
      // 非webm格式，直接使用原始文件
      final_container_path =
          "/root/llonebot/bridge_files/animations/original/" +
          original_filename;
    }

    // 只有在有 file_unique_id 时才保存到数据库
    if (!media_info.file_unique_id.empty()) {
      // 创建缓存信息
      storage::StickerCacheInfo new_cache_info;
      new_cache_info.platform = "telegram_animation";
      new_cache_info.sticker_id = media_info.file_id; // 原始file_id
      new_cache_info.sticker_hash =
          media_info.file_unique_id; // 用于查询的唯一ID
      new_cache_info.original_file_path = original_file_path; // 主机路径
      new_cache_info.file_size = file_content.size();
      new_cache_info.mime_type = mime_type;
      new_cache_info.conversion_status = conversion_status;
      new_cache_info.created_at = std::chrono::system_clock::now();
      new_cache_info.last_used_at = std::chrono::system_clock::now();
      new_cache_info.container_path = final_container_path; // 容器内路径

      // 如果有转换后的文件，也保存转换后的路径
      if (final_file_path != original_file_path) {
        new_cache_info.converted_file_path = final_file_path;
      }

      // 保存到缓存数据库
      if (!db_manager_->save_sticker_cache(new_cache_info)) {
        PLUGIN_WARN("tg_to_qq", "保存动画缓存失败，但文件已下载: {}",
                    final_file_path);
      }
    } else {
      PLUGIN_DEBUG("tg_to_qq", "没有file_unique_id，跳过数据库保存");
    }

    PLUGIN_INFO("tg_to_qq", "动画缓存完成: {} -> {}", media_info.file_id,
                final_container_path);
    co_return final_container_path;

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "下载动画失败 (file_id: {}): {}",
                 media_info.file_id, e.what());
    co_return std::nullopt;
  }
}

} // namespace bridge::telegram

#include "telegram/media_processor.hpp"

#include "config.hpp"

#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <media_converter.hpp>
#include <telegram/network/connection_manager.hpp>

namespace bridge::telegram {

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

auto TelegramMediaProcessor::download_sticker_with_cache(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const std::string &bridge_files_dir)
    -> boost::asio::awaitable<std::optional<std::string>> {

  try {
    if (media_info.file_type != "sticker") {
      PLUGIN_ERROR("tg_to_qq", "不支持的文件类型，仅支持sticker: {}",
                   media_info.file_type);
      co_return std::nullopt;
    }

    // 严格使用 file_unique_id 作为唯一缓存键，不要使用任何 hash
    if (media_info.file_unique_id.empty()) {
      PLUGIN_WARN("tg_to_qq",
                  "file_unique_id为空，跳过数据库缓存操作，直接下载: {}",
                  media_info.file_id);
    } else {
      std::string cache_key = media_info.file_unique_id;
      PLUGIN_DEBUG("tg_to_qq", "表情包缓存查找，使用file_unique_id: {}",
                   cache_key);

      auto cache_info = db_manager_->get_sticker_cache("telegram", cache_key);
      if (cache_info.has_value()) {
        // 缓存命中也要校验文件实际存在，否则视为失效需重新下载
        bool file_exists = false;

        if (cache_info->conversion_status == "success" &&
            cache_info->converted_file_path.has_value()) {
          // 转换成功时优先用转换后文件，否则回退到原始文件
          std::string host_path = cache_info->converted_file_path.value();
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        } else if (!cache_info->original_file_path.empty()) {
          std::string host_path = cache_info->original_file_path;
          if (std::filesystem::exists(host_path)) {
            file_exists = true;
          }
        }

        if (file_exists && !cache_info->container_path.empty()) {
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

    PLUGIN_INFO("tg_to_qq", "表情包缓存未命中，开始下载: {}",
                media_info.file_id);

    auto download_urls =
        co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
            .get_media_download_urls({media_info});
    if (download_urls.empty() || !download_urls[0].has_value()) {
      PLUGIN_ERROR("tg_to_qq", "获取表情包下载URL失败: {}", media_info.file_id);
      co_return std::nullopt;
    }

    std::string download_url = download_urls[0].value();

    std::string host_bridge_files_dir = bridge::config::BRIDGE_FILES_DIR;
    std::string original_dir = host_bridge_files_dir + "/stickers/original";
    std::filesystem::create_directories(original_dir);

    std::string file_extension = ".webp";
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

    std::string filename_prefix;
    if (!media_info.file_unique_id.empty()) {
      filename_prefix =
          fmt::format("sticker_{}_{}", media_info.file_unique_id.substr(0, 12),
                      media_info.file_id.substr(0, 8));
    } else {
      auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      filename_prefix = fmt::format("sticker_{}_{}", timestamp,
                                    media_info.file_id.substr(0, 8));
    }
    std::string original_filename = filename_prefix + file_extension;
    std::string original_file_path = original_dir + "/" + original_filename;

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

    // QQ 不能直接显示 webm 贴纸，必须先转 gif；
    // 转换受 GIF_MAX_DURATION/SIZE/WIDTH/FPS/COLORS 等参数约束
    if (mime_type == "video/webm" || file_extension == ".webm") {
      PLUGIN_INFO("tg_to_qq", "检测到webm格式贴纸，开始转换为gif: {}",
                  original_file_path);

      std::string converted_dir = host_bridge_files_dir + "/stickers/converted";
      std::filesystem::create_directories(converted_dir);

      std::string converted_filename = filename_prefix + ".gif";
      std::string converted_file_path =
          converted_dir + "/" + converted_filename;

      bool conversion_success =
          bridge::MediaConverter::convert_webm_to_gif_with_fallback(
              original_file_path, converted_file_path,
              bridge::config::GIF_MAX_DURATION,
              bridge::config::GIF_MAX_FILE_SIZE, bridge::config::GIF_MAX_WIDTH,
              bridge::config::GIF_MAX_FPS, bridge::config::GIF_MAX_COLORS);

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
      final_container_path =
          "/root/llonebot/bridge_files/stickers/original/" + original_filename;
    }

    // 缓存条目以 file_unique_id 作为查询键；没有它就跳过持久化
    if (!media_info.file_unique_id.empty()) {
      storage::StickerCacheInfo new_cache_info;
      new_cache_info.platform = "telegram";
      new_cache_info.sticker_id = media_info.file_id;
      new_cache_info.sticker_hash = media_info.file_unique_id;
      new_cache_info.original_file_path = original_file_path;
      new_cache_info.file_size = file_content.size();
      new_cache_info.mime_type = mime_type;
      new_cache_info.conversion_status = conversion_status;
      new_cache_info.created_at = std::chrono::system_clock::now();
      new_cache_info.last_used_at = std::chrono::system_clock::now();
      new_cache_info.container_path = final_container_path;

      if (final_file_path != original_file_path) {
        new_cache_info.converted_file_path = final_file_path;
      }

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

} // namespace bridge::telegram

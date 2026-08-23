#include "telegram/media_processor.hpp"

#include "bridge_state_repository.hpp"
#include "config.hpp"

#include <common/logger.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <media_converter.hpp>

namespace bridge::telegram {

auto TelegramMediaProcessor::process_sticker(
    const obcx::bot::TelegramFileRef &media_info,
    const nlohmann::json &media_data)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto cached_path_opt =
        co_await download_sticker_with_cache(media_info, "/tmp/bridge_files");

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

      OBCX_INFO("成功缓存Telegram sticker到容器路径: {}", container_file_path);
    } else {
      throw std::runtime_error("缓存下载失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("缓存系统处理表情包失败: {}, 回退为文本提示", e.what());

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
    const obcx::bot::TelegramFileRef &media_info,
    const std::string &bridge_files_dir)
    -> boost::asio::awaitable<std::optional<std::string>> {

  try {
    if (media_info.file_type != "sticker") {
      OBCX_ERROR("不支持的文件类型，仅支持sticker: {}", media_info.file_type);
      co_return std::nullopt;
    }

    // 严格使用 file_unique_id 作为唯一缓存键，不要使用任何 hash
    if (media_info.file_unique_id.empty()) {
      OBCX_WARN("file_unique_id为空，跳过数据库缓存操作，直接下载: {}",
                media_info.file_id);
    } else {
      std::string cache_key = media_info.file_unique_id;
      OBCX_DEBUG("表情包缓存查找，使用file_unique_id: {}", cache_key);

      const auto installation =
          operations_->telegram_installation().installation_id;
      auto cached_path = co_await blocking_executor_->run(
          [repository = state_repository_, installation,
           cache_key] -> std::optional<std::string> {
            if (!repository) {
              return std::nullopt;
            }
            auto cache_info = repository->get_sticker_cache(
                installation, "telegram", cache_key);
            if (!cache_info.has_value()) {
              return std::nullopt;
            }
            const auto host_path =
                cache_info->conversion_status == "success" &&
                        cache_info->converted_file_path.has_value()
                    ? *cache_info->converted_file_path
                    : cache_info->original_file_path;
            if (host_path.empty() || !std::filesystem::exists(host_path) ||
                cache_info->container_path.empty()) {
              return std::nullopt;
            }
            cache_info->last_used_at = std::chrono::system_clock::now();
            (void)repository->save_sticker_cache(*cache_info);
            return cache_info->container_path;
          });
      if (cached_path.has_value()) {
        OBCX_DEBUG("表情包缓存命中: {} -> {}", cache_key, *cached_path);
        co_return cached_path;
      }
      OBCX_WARN("表情包缓存未命中或文件丢失，将重新下载: {}", cache_key);
    }

    OBCX_INFO("表情包缓存未命中，开始下载: {}", media_info.file_id);

    const std::string &host_bridge_files_dir = config_->bridge_files_dir;
    std::string original_dir = host_bridge_files_dir + "/stickers/original";

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

    const auto fetched = co_await operations_->fetch_telegram_file(
        media_info, config_->qq_media_download_max_bytes);
    std::string file_content(fetched.bytes.begin(), fetched.bytes.end());

    const auto downloaded_size = file_content.size();
    co_await blocking_executor_->run([original_dir, original_file_path,
                                      file_content = std::move(file_content)] {
      std::filesystem::create_directories(original_dir);
      std::ofstream file(original_file_path, std::ios::binary);
      if (!file) {
        throw std::runtime_error("无法创建文件: " + original_file_path);
      }
      file.write(file_content.data(), file_content.size());
      file.close();
      if (!file) {
        throw std::runtime_error("写入文件失败: " + original_file_path);
      }
    });

    OBCX_INFO("表情包原始文件已下载: {} -> {} ({}字节)", media_info.file_id,
              original_file_path, downloaded_size);

    std::string final_file_path = original_file_path;
    std::string final_container_path;
    std::string conversion_status = "success";

    // QQ 不能直接显示 webm 贴纸，必须先转 gif；
    // 转换受 GIF_MAX_DURATION/SIZE/WIDTH/FPS/COLORS 等参数约束
    if (mime_type == "video/webm" || file_extension == ".webm") {
      OBCX_INFO("检测到webm格式贴纸，开始转换为gif: {}", original_file_path);

      std::string converted_dir = host_bridge_files_dir + "/stickers/converted";

      std::string converted_filename = filename_prefix + ".gif";
      std::string converted_file_path =
          converted_dir + "/" + converted_filename;

      const auto config = config_;
      const bool conversion_success = co_await blocking_executor_->run(
          [config, original_file_path, converted_dir, converted_file_path] {
            std::filesystem::create_directories(converted_dir);
            return bridge::MediaConverter::convert_webm_to_gif_with_fallback(
                       config->ffmpeg_path, original_file_path,
                       converted_file_path, config->gif_max_duration,
                       config->gif_max_file_size, config->gif_max_width,
                       config->gif_max_fps, config->gif_max_colors) &&
                   std::filesystem::exists(converted_file_path);
          });

      if (conversion_success) {
        OBCX_INFO("webm贴纸到gif转换成功: {} -> {}", original_file_path,
                  converted_file_path);
        final_file_path = converted_file_path;
        final_container_path =
            "/root/llonebot/bridge_files/stickers/converted/" +
            converted_filename;
        conversion_status = "success";
      } else {
        OBCX_WARN("webm贴纸到gif转换失败，使用原始webm文件: {}",
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
      new_cache_info.installation_id =
          operations_->telegram_installation().installation_id;
      new_cache_info.platform = "telegram";
      new_cache_info.sticker_id = media_info.file_id;
      new_cache_info.sticker_hash = media_info.file_unique_id;
      new_cache_info.original_file_path = original_file_path;
      new_cache_info.file_size = downloaded_size;
      new_cache_info.mime_type = mime_type;
      new_cache_info.conversion_status = conversion_status;
      new_cache_info.created_at = std::chrono::system_clock::now();
      new_cache_info.last_used_at = std::chrono::system_clock::now();
      new_cache_info.container_path = final_container_path;

      if (final_file_path != original_file_path) {
        new_cache_info.converted_file_path = final_file_path;
      }

      const auto saved =
          !state_repository_ ||
          co_await blocking_executor_->run(
              [repository = state_repository_, new_cache_info] {
                return repository->save_sticker_cache(new_cache_info);
              });
      if (!saved) {
        OBCX_WARN("保存表情包缓存失败，但文件已下载: {}", final_file_path);
      }
    } else {
      OBCX_DEBUG("没有file_unique_id，跳过数据库保存");
    }

    OBCX_INFO("表情包缓存完成: {} -> {}", media_info.file_id,
              final_container_path);
    co_return final_container_path;

  } catch (const std::exception &e) {
    OBCX_ERROR("下载表情包失败 (file_id: {}): {}", media_info.file_id,
               e.what());
    co_return std::nullopt;
  }
}

} // namespace bridge::telegram

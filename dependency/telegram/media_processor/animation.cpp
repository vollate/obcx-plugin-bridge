#include "telegram/media_processor.hpp"

#include "bridge_state_repository.hpp"
#include "config.hpp"
#include "media_processor.hpp"

#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <media_converter.hpp>
#include <telegram/network/connection_manager.hpp>

namespace bridge::telegram {

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

      OBCX_COMPONENT_INFO("tg_to_qq",
                          "成功缓存Telegram animation到容器路径: {}",
                          container_file_path);
    } else {
      throw std::runtime_error("缓存下载失败");
    }
  } catch (const std::exception &e) {
    OBCX_COMPONENT_WARN("tg_to_qq", "缓存系统处理动画失败: {}, 回退到URL方式",
                        e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info("", "animation");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::download_animation_with_cache(
    obcx::core::IBot &telegram_bot, const obcx::core::MediaFileInfo &media_info,
    const std::string &bridge_files_dir)
    -> boost::asio::awaitable<std::optional<std::string>> {

  try {
    if (media_info.file_type != "animation") {
      OBCX_COMPONENT_ERROR("tg_to_qq", "不支持的文件类型，仅支持animation: {}",
                           media_info.file_type);
      co_return std::nullopt;
    }

    // 严格使用 file_unique_id 作为唯一缓存键，不要使用任何 hash
    if (media_info.file_unique_id.empty()) {
      OBCX_COMPONENT_WARN(
          "tg_to_qq", "file_unique_id为空，跳过数据库缓存操作，直接下载: {}",
          media_info.file_id);
    } else {
      std::string cache_key = media_info.file_unique_id;
      OBCX_COMPONENT_DEBUG("tg_to_qq", "动画缓存查找，使用file_unique_id: {}",
                           cache_key);

      auto cache_info = state_repository_
                            ? state_repository_->get_sticker_cache(
                                  "telegram_animation", cache_key)
                            : std::optional<storage::StickerCacheInfo>{};
      if (cache_info.has_value()) {
        // 缓存命中也要校验文件实际存在，否则视为失效需重新下载
        bool file_exists = false;

        if (cache_info->conversion_status == "success" &&
            cache_info->converted_file_path.has_value()) {
          // 转换成功时优先用 gif，否则回退到原始文件
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
          if (state_repository_) {
            state_repository_->save_sticker_cache(update_info);
          }

          OBCX_COMPONENT_DEBUG("tg_to_qq", "动画缓存命中: {} -> {}", cache_key,
                               cache_info->container_path);
          co_return cache_info->container_path;
        } else {
          OBCX_COMPONENT_WARN("tg_to_qq",
                              "动画缓存项存在但文件丢失，将重新下载: {}",
                              cache_key);
        }
      }
    }

    OBCX_COMPONENT_INFO("tg_to_qq", "动画缓存未命中，开始下载: {}",
                        media_info.file_id);

    auto download_urls =
        co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
            .get_media_download_urls({media_info});
    if (download_urls.empty() || !download_urls[0].has_value()) {
      OBCX_COMPONENT_ERROR("tg_to_qq", "获取动画下载URL失败: {}",
                           media_info.file_id);
      co_return std::nullopt;
    }

    std::string download_url = download_urls[0].value();

    std::string host_bridge_files_dir = bridge::config::BRIDGE_FILES_DIR;
    std::string original_dir = host_bridge_files_dir + "/animations/original";
    std::string converted_dir = host_bridge_files_dir + "/animations/converted";
    std::filesystem::create_directories(original_dir);
    std::filesystem::create_directories(converted_dir);

    std::string file_extension = ".mp4";
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

    std::string filename_prefix;
    if (!media_info.file_unique_id.empty()) {
      filename_prefix = fmt::format("animation_{}_{}",
                                    media_info.file_unique_id.substr(0, 12),
                                    media_info.file_id.substr(0, 8));
    } else {
      auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      filename_prefix = fmt::format("animation_{}_{}", timestamp,
                                    media_info.file_id.substr(0, 8));
    }
    std::string original_filename = filename_prefix + file_extension;
    std::string original_file_path = original_dir + "/" + original_filename;

    auto *tg_bot = dynamic_cast<obcx::core::TGBot *>(&telegram_bot);
    if (!tg_bot) {
      OBCX_COMPONENT_ERROR("tg_to_qq", "telegram_bot不是TGBot类型");
      co_return std::nullopt;
    }

    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      OBCX_COMPONENT_ERROR("tg_to_qq",
                           "连接管理器不是TelegramConnectionManager类型");
      co_return std::nullopt;
    }

    auto file_content =
        co_await conn_manager->download_file_content(download_url);
    if (file_content.empty()) {
      OBCX_COMPONENT_ERROR("tg_to_qq", "下载文件内容为空: {}", download_url);
      co_return std::nullopt;
    }

    std::ofstream file(original_file_path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("无法创建文件: " + original_file_path);
    }
    file.write(file_content.data(), file_content.size());
    file.close();

    OBCX_COMPONENT_INFO("tg_to_qq", "动画原始文件已下载: {} -> {} ({}字节)",
                        media_info.file_id, original_file_path,
                        file_content.size());

    std::string final_file_path = original_file_path;
    std::string final_container_path;
    std::string conversion_status = "success";

    // QQ 端不能直接发 webm 动图，必须先转成 gif；
    // 转换受 GIF_MAX_DURATION/SIZE/WIDTH/FPS/COLORS 等参数约束
    if (mime_type == "video/webm" || file_extension == ".webm") {
      OBCX_COMPONENT_INFO("tg_to_qq", "检测到webm格式动画，开始转换为gif: {}",
                          original_file_path);

      std::string converted_filename = filename_prefix + ".gif";
      std::string converted_file_path =
          converted_dir + "/" + converted_filename;
      if (!std::filesystem::exists(converted_dir)) {
        std::filesystem::create_directories(converted_dir);
      }

      bool conversion_success =
          bridge::MediaConverter::convert_webm_to_gif_with_fallback(
              original_file_path, converted_file_path,
              bridge::config::GIF_MAX_DURATION,
              bridge::config::GIF_MAX_FILE_SIZE, bridge::config::GIF_MAX_WIDTH,
              bridge::config::GIF_MAX_FPS, bridge::config::GIF_MAX_COLORS);

      if (conversion_success && std::filesystem::exists(converted_file_path)) {
        OBCX_COMPONENT_INFO("tg_to_qq", "webm到gif转换成功: {} -> {}",
                            original_file_path, converted_file_path);
        final_file_path = converted_file_path;
        final_container_path =
            "/root/llonebot/bridge_files/animations/converted/" +
            converted_filename;
        conversion_status = "success";
      } else {
        OBCX_COMPONENT_WARN("tg_to_qq",
                            "webm到gif转换失败，使用原始webm文件: {}",
                            original_file_path);
        final_container_path =
            "/root/llonebot/bridge_files/animations/original/" +
            original_filename;
        conversion_status = "failed";
      }
    } else {
      final_container_path =
          "/root/llonebot/bridge_files/animations/original/" +
          original_filename;
    }

    // 缓存条目以 file_unique_id 作为查询键；没有它就跳过持久化
    if (!media_info.file_unique_id.empty()) {
      storage::StickerCacheInfo new_cache_info;
      new_cache_info.platform = "telegram_animation";
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

      if (state_repository_ &&
          !state_repository_->save_sticker_cache(new_cache_info)) {
        OBCX_COMPONENT_WARN("tg_to_qq", "保存动画缓存失败，但文件已下载: {}",
                            final_file_path);
      }
    } else {
      OBCX_COMPONENT_DEBUG("tg_to_qq", "没有file_unique_id，跳过数据库保存");
    }

    OBCX_COMPONENT_INFO("tg_to_qq", "动画缓存完成: {} -> {}",
                        media_info.file_id, final_container_path);
    co_return final_container_path;

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR("tg_to_qq", "下载动画失败 (file_id: {}): {}",
                         media_info.file_id, e.what());
    co_return std::nullopt;
  }
}

} // namespace bridge::telegram

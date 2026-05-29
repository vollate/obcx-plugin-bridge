// Telegram媒体处理器：animation 消息段转换、下载、缓存和 webm 转 gif。

#include "telegram/media_processor.hpp"

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
    auto download_urls =
        co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
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
          bridge::MediaConverter::convert_webm_to_gif_with_fallback(
              original_file_path, converted_file_path,
              bridge::config::GIF_MAX_DURATION,
              bridge::config::GIF_MAX_FILE_SIZE, bridge::config::GIF_MAX_WIDTH,
              bridge::config::GIF_MAX_FPS, bridge::config::GIF_MAX_COLORS);

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

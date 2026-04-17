#include "media_processor.hpp"
#include "config.hpp"
#include "path_manager.hpp"

#include <algorithm>
#include <cctype>
#include <common/logger.hpp>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>

namespace bridge {

auto MediaProcessor::get_qq_file_info(const std::string &file_url,
                                      const std::string &file_type)
    -> std::pair<std::string, std::string> {
  try {
    // 从URL中提取文件名
    std::string filename = "file";
    size_t pos = file_url.find_last_of("/");
    if (pos != std::string::npos && pos + 1 < file_url.length()) {
      filename = file_url.substr(pos + 1);
      // 移除查询参数
      size_t query_pos = filename.find("?");
      if (query_pos != std::string::npos) {
        filename = filename.substr(0, query_pos);
      }
    }

    // 根据文件类型添加扩展名
    if (filename.find('.') == std::string::npos) {
      if (file_type == "photo" || file_type == "image") {
        filename += ".jpg";
      } else if (file_type == "video") {
        filename += ".mp4";
      } else if (file_type == "audio" || file_type == "voice") {
        filename += ".mp3";
      } else if (file_type == "sticker") {
        filename += ".webp";
      } else if (file_type == "document") {
        filename += ".file";
      } else if (file_type == "animation") {
        filename += ".gif";
      } else if (file_type == "video_note") {
        filename += ".mp4";
      }
    }

    return {file_url, filename};
  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge", "获取QQ文件信息时出错: {}", e.what());
    return {file_url, "file"};
  }
}

auto MediaProcessor::cleanup_media_file(const std::string &file_path) -> void {
  try {
    if (std::filesystem::exists(file_path)) {
      std::filesystem::remove(file_path);
      PLUGIN_DEBUG("bridge", "清理临时媒体文件: {}", file_path);
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge", "清理临时媒体文件失败: {}, 错误: {}", file_path,
                 e.what());
  }
}

auto MediaProcessor::get_path_manager() -> const PathManager & {
  // 使用单例模式，延迟初始化
  static std::unique_ptr<PathManager> path_manager_instance;
  static std::once_flag init_flag;

  std::call_once(init_flag, []() {
    if (bridge::config::BRIDGE_FILES_DIR.empty()) {
      throw std::runtime_error(
          "PathManager: BRIDGE_FILES_DIR is not configured. Please ensure "
          "initialize_config() is called before using MediaProcessor.");
    }
    path_manager_instance = std::make_unique<PathManager>(
        bridge::config::BRIDGE_FILES_DIR,
        bridge::config::BRIDGE_FILES_CONTAINER_DIR);
  });

  return *path_manager_instance;
}

auto MediaProcessor::is_gif_content_type(const std::string &content_type)
    -> bool {
  if (content_type.empty()) {
    return false;
  }

  // 转换为小写比较
  std::string lower_content_type = content_type;
  std::transform(lower_content_type.begin(), lower_content_type.end(),
                 lower_content_type.begin(), ::tolower);

  // 检测GIF类型
  bool is_gif = (lower_content_type == "image/gif" ||
                 lower_content_type == "image/x-gif" ||
                 lower_content_type.find("gif") != std::string::npos);

  PLUGIN_DEBUG("bridge", "Content-Type {} 判断为GIF: {}", content_type, is_gif);
  return is_gif;
}

auto MediaProcessor::detect_mime_type_from_content(const std::string &content)
    -> std::string {
  if (content.empty()) {
    return "";
  }

  // 检查文件头标识（Magic Numbers）
  const unsigned char *data =
      reinterpret_cast<const unsigned char *>(content.data());
  size_t size = content.size();

  // GIF: "GIF87a" 或 "GIF89a"
  if (size >= 6 && data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46 &&
      data[3] == 0x38 && (data[4] == 0x37 || data[4] == 0x39) &&
      data[5] == 0x61) {
    return "image/gif";
  }

  // JPEG: FF D8 FF
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return "image/jpeg";
  }

  // PNG: 89 50 4E 47 0D 0A 1A 0A
  if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
      data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A &&
      data[6] == 0x1A && data[7] == 0x0A) {
    return "image/png";
  }

  // WebP: "RIFF" ... "WEBP"
  if (size >= 12 && data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 &&
      data[3] == 0x46 && data[8] == 0x57 && data[9] == 0x45 &&
      data[10] == 0x42 && data[11] == 0x50) {
    return "image/webp";
  }

  // BMP: "BM"
  if (size >= 2 && data[0] == 0x42 && data[1] == 0x4D) {
    return "image/bmp";
  }

  // 默认返回空字符串表示未知类型
  return "";
}

auto MediaProcessor::is_gif_from_content(const std::string &content) -> bool {
  std::string mime_type = detect_mime_type_from_content(content);
  return mime_type == "image/gif";
}

} // namespace bridge

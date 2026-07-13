#include "media_converter.hpp"
#include <common/logger.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>

namespace bridge {

namespace {
constexpr const char *LOG_TAG = "bridge";
} // namespace

auto MediaConverter::convert_webm_to_gif(const std::string &webm_path,
                                         const std::string &output_path,
                                         int max_duration, int max_width,
                                         int max_fps, int max_colors) -> bool {
  try {
    OBCX_COMPONENT_INFO(LOG_TAG, "开始WebM到GIF转换: {} -> {}", webm_path,
                        output_path);

    if (max_colors < 2) {
      max_colors = 2;
    }
    if (max_colors > 256) {
      max_colors = 256;
    }

    std::ostringstream filter;

    // fps filter must come before split (both palette branches need same fps)
    if (max_fps > 0) {
      filter << "fps=fps=" << max_fps << ",";
    }

    if (max_width > 0) {
      filter << "scale=" << max_width
             << ":-1:flags=lanczos:force_original_aspect_ratio=decrease,";
    }

    // palettegen builds an optimal palette (reserve_transparent keeps alpha),
    // paletteuse then maps frames onto it
    filter << "split[s0][s1];"
           << "[s0]palettegen=reserve_transparent=on"
           << ":max_colors=" << max_colors << ":stats_mode=full[p];"
           << "[s1][p]paletteuse=dither=bayer"
           << ":bayer_scale=5:diff_mode=rectangle";

    std::ostringstream cmd;
    cmd << "ffmpeg -i \"" << webm_path << "\" "
        << "-t " << max_duration << " "
        << "-vf \"" << filter.str() << "\" "
        << "-loop 0 "
        << "-y \"" << output_path << "\" "
        << "2>/dev/null";

    OBCX_COMPONENT_DEBUG(LOG_TAG, "执行ffmpeg命令: {}", cmd.str());

    bool success = execute_command(cmd.str());

    if (success && is_valid_file(output_path)) {
      auto file_size = std::filesystem::file_size(output_path);
      OBCX_COMPONENT_INFO(LOG_TAG, "WebM到GIF转换成功, 输出文件大小: {} 字节",
                          file_size);
      return true;
    }

    OBCX_COMPONENT_ERROR(LOG_TAG, "WebM到GIF转换失败");
    return false;

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "WebM到GIF转换异常: {}", e.what());
    return false;
  }
}

auto MediaConverter::convert_webm_to_gif_async(const std::string &webm_path,
                                               const std::string &output_path,
                                               int max_duration, int max_width,
                                               int max_fps, int max_colors)
    -> std::future<bool> {
  return std::async(std::launch::async, [=]() -> bool {
    return convert_webm_to_gif(webm_path, output_path, max_duration, max_width,
                               max_fps, max_colors);
  });
}

auto MediaConverter::convert_webm_to_gif_with_fallback(
    const std::string &webm_path, const std::string &output_path,
    int max_duration, size_t max_file_size, int max_width, int max_fps,
    int max_colors) -> bool {
  try {
    OBCX_COMPONENT_INFO(
        LOG_TAG,
        "开始WebM到GIF转换(带回退), 输入: {}, 输出: {}, 大小限制: {} 字节",
        webm_path, output_path,
        max_file_size == 0 ? "无限制" : std::to_string(max_file_size));

    // Define compression tiers: {name, width (0=original), fps (0=original),
    // colors}
    struct Tier {
      const char *name;
      int max_width;
      int max_fps;
      int max_colors;
    };

    const Tier tiers[] = {
        {"quality", max_width, max_fps, max_colors}, // Default: old lossless
        {"balanced", 320, 0, 256}, // Matches the old compressed fallback
        {"compact", 200, 8, 64},   // Aggressive compression
        {"minimal", 160, 5, 32},   // Maximum compression
    };

    for (const auto &tier : tiers) {
      OBCX_COMPONENT_INFO(
          LOG_TAG, "尝试 [{}] 级别转换 (宽度={}, fps={}, 颜色={})", tier.name,
          tier.max_width == 0 ? "原始" : std::to_string(tier.max_width),
          tier.max_fps == 0 ? "原始" : std::to_string(tier.max_fps),
          tier.max_colors);

      bool success =
          convert_webm_to_gif(webm_path, output_path, max_duration,
                              tier.max_width, tier.max_fps, tier.max_colors);

      if (!success || !is_valid_file(output_path)) {
        OBCX_COMPONENT_WARN(LOG_TAG, "[{}] 级别转换失败, 尝试下一级",
                            tier.name);
        cleanup_temp_file(output_path);
        continue;
      }

      auto file_size = std::filesystem::file_size(output_path);

      if (max_file_size > 0 && file_size > max_file_size) {
        OBCX_COMPONENT_WARN(
            LOG_TAG, "[{}] 级别输出过大 ({} 字节 > {} 字节限制), 尝试下一级",
            tier.name, file_size, max_file_size);
        cleanup_temp_file(output_path);
        continue;
      }

      OBCX_COMPONENT_INFO(LOG_TAG, "[{}] 级别转换成功, 输出大小: {} 字节",
                          tier.name, file_size);
      return true;
    }

    OBCX_COMPONENT_ERROR(LOG_TAG, "所有转换级别均失败");
    return false;

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "WebM到GIF回退转换异常: {}", e.what());
    cleanup_temp_file(output_path);
    return false;
  }
}

auto MediaConverter::convert_tgs_to_gif(const std::string &tgs_path,
                                        const std::string &output_path,
                                        int max_width) -> bool {
  try {
    OBCX_COMPONENT_INFO(LOG_TAG, "开始TGS到GIF转换: {} -> {}", tgs_path,
                        output_path);

    std::ostringstream cmd;
    cmd << "lottie_convert.py \"" << tgs_path << "\" \"" << output_path << "\" "
        << "--width " << max_width << " --height " << max_width << " "
        << "2>/dev/null";

    OBCX_COMPONENT_DEBUG(LOG_TAG, "执行TGS转换命令: {}", cmd.str());

    bool success = execute_command(cmd.str());

    if (success && is_valid_file(output_path)) {
      auto file_size = std::filesystem::file_size(output_path);
      OBCX_COMPONENT_INFO(LOG_TAG, "TGS到GIF转换成功, 输出大小: {} 字节",
                          file_size);
      return true;
    }

    OBCX_COMPONENT_WARN(LOG_TAG, "TGS到GIF转换失败");
    return false;

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "TGS到GIF转换异常: {}", e.what());
    return false;
  }
}

auto MediaConverter::generate_temp_path(const std::string &extension)
    -> std::string {
  try {
    const char *env_dir = std::getenv("OBCX_BRIDGE_FILES_DIR");
    std::filesystem::path shared_dir = env_dir ? env_dir : "/tmp/bridge_files";

    std::filesystem::create_directories(shared_dir);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);

    std::string filename =
        "convert_" + std::to_string(dis(gen)) + "." + extension;
    std::filesystem::path temp_file = shared_dir / filename;

    OBCX_COMPONENT_DEBUG(LOG_TAG, "生成临时文件路径: {}", temp_file.string());
    return temp_file.string();

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "生成临时文件路径失败: {}", e.what());
    throw;
  }
}

auto MediaConverter::cleanup_temp_file(const std::string &file_path) -> void {
  try {
    if (std::filesystem::exists(file_path)) {
      std::filesystem::remove(file_path);
      OBCX_COMPONENT_DEBUG(LOG_TAG, "已清理临时文件: {}", file_path);
    }
  } catch (const std::exception &e) {
    OBCX_COMPONENT_WARN(LOG_TAG, "清理临时文件失败: {} - {}", file_path,
                        e.what());
  }
}

auto MediaConverter::execute_command(const std::string &command) -> bool {
  try {
    OBCX_COMPONENT_DEBUG(LOG_TAG, "执行命令: {}", command);
    int result = std::system(command.c_str());
    bool success = (result == 0);

    if (success) {
      OBCX_COMPONENT_DEBUG(LOG_TAG, "命令执行成功");
    } else {
      OBCX_COMPONENT_DEBUG(LOG_TAG, "命令执行失败, 返回码: {}", result);
    }

    return success;
  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "执行命令异常: {}", e.what());
    return false;
  }
}

auto MediaConverter::is_valid_file(const std::string &file_path) -> bool {
  try {
    if (!std::filesystem::exists(file_path)) {
      OBCX_COMPONENT_DEBUG(LOG_TAG, "文件不存在: {}", file_path);
      return false;
    }

    auto file_size = std::filesystem::file_size(file_path);
    if (file_size == 0) {
      OBCX_COMPONENT_DEBUG(LOG_TAG, "文件为空: {}", file_path);
      return false;
    }

    OBCX_COMPONENT_DEBUG(LOG_TAG, "文件有效: {} ({} 字节)", file_path,
                         file_size);
    return true;

  } catch (const std::exception &e) {
    OBCX_COMPONENT_ERROR(LOG_TAG, "检查文件异常: {} - {}", file_path, e.what());
    return false;
  }
}

} // namespace bridge

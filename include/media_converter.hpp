#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace bridge {

/**
 * @brief Media format converter for the bridge actor
 *
 * Converts media files between formats, particularly Telegram-specific
 * formats to QQ-compatible formats. Supports multi-tier fallback with
 * file size constraints to prevent upload failures.
 */
class MediaConverter {
public:
  /// Default maximum output file size (0 = unlimited, preserve old lossless
  /// default)
  static constexpr size_t DEFAULT_MAX_FILE_SIZE = 0;

  /**
   * @brief Convert WebM to GIF with configurable quality parameters
   * @param webm_path Path to WebM input file
   * @param output_path Output GIF file path
   * @param max_duration Maximum conversion duration in seconds (default 5)
   * @param max_width Maximum output width (0 = keep original resolution)
   * @param max_fps Maximum frame rate (0 = keep original frame rate)
   * @param max_colors Maximum palette colors (1-256, default 256)
   * @return true if conversion succeeded and output file is valid
   */
  static auto convert_webm_to_gif(std::string_view ffmpeg_path,
                                  const std::string &webm_path,
                                  const std::string &output_path,
                                  int max_duration = 5, int max_width = 0,
                                  int max_fps = 0, int max_colors = 256)
      -> bool;

  /**
   * @brief Convert WebM to GIF with multi-tier fallback and size constraint
   *
   * Tries progressively more aggressive compression tiers:
   *   1. Quality:  configured/default limits (defaults to original size/fps,
   *      256 colors)
   *   2. Balanced: 320px width, original fps, 256 colors
   *   3. Compact:  200px width, 8fps, 64 colors
   *   4. Minimal:  160px width, 5fps, 32 colors
   *
   * After each tier, checks if output exceeds max_file_size. If so,
   * deletes the output and tries the next tier.
   *
   * @param webm_path Path to WebM input file
   * @param output_path Output GIF file path
   * @param max_duration Maximum conversion duration in seconds (default 5)
   * @param max_file_size Maximum acceptable output size in bytes (0 =
   * unlimited)
   * @param max_width Maximum output width (0 = keep original resolution)
   * @param max_fps Maximum frame rate (0 = keep original frame rate)
   * @param max_colors Maximum palette colors (2-256)
   * @return true if any tier produced a valid file within the size limit
   */
  static auto convert_webm_to_gif_with_fallback(
      std::string_view ffmpeg_path, const std::string &webm_path,
      const std::string &output_path, int max_duration = 5,
      size_t max_file_size = DEFAULT_MAX_FILE_SIZE, int max_width = 0,
      int max_fps = 0, int max_colors = 256) -> bool;

  /**
   * @brief Convert TGS (Telegram animated sticker) to GIF
   * @param tgs_path Path to TGS input file
   * @param output_path Output GIF file path
   * @param max_width Maximum output width (default 512)
   * @return true if conversion succeeded
   */
  static auto convert_tgs_to_gif(const std::string &tgs_path,
                                 const std::string &output_path,
                                 int max_width = 512) -> bool;

  /**
   * @brief Generate a temporary file path in the shared bridge files directory
   * @param extension File extension without dot (e.g. "gif")
   * @return Full path to a temporary file
   */
  static auto generate_temp_path(const std::string &extension) -> std::string;

  /**
   * @brief Delete a temporary file if it exists
   * @param file_path Path to the file to remove
   */
  static auto cleanup_temp_file(const std::string &file_path) -> void;

private:
  static auto execute_command(const std::string &command) -> bool;
  static auto is_valid_file(const std::string &file_path) -> bool;
};

} // namespace bridge

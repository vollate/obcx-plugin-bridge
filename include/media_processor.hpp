#pragma once

#include "path_manager.hpp"

#include <boost/asio.hpp>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

namespace bridge {

/**
 * @brief 媒体文件处理器
 *
 * 处理QQ和Telegram之间的媒体文件转换和下载
 */
class MediaProcessor {
public:
  /**
   * @brief 获取QQ文件信息（文件名和URL）
   * @param file_url 文件URL
   * @param file_type 文件类型
   * @return {URL, 文件名} 的pair
   */
  static auto get_qq_file_info(const std::string &file_url,
                               const std::string &file_type = "image")
      -> std::pair<std::string, std::string>;

  /**
   * @brief 下载媒体文件到本地临时目录
   * @param file_url 文件下载URL
   * @param file_type 文件类型
   * @param filename 目标文件名（可选）
   * @return 下载后的本地文件路径的awaitable，失败时返回nullopt
   */
  template <typename ConnectionManager>
  static auto download_media_file(ConnectionManager *conn_manager,
                                  const std::string &file_url,
                                  const std::string &file_type,
                                  const std::string &filename = "")
      -> boost::asio::awaitable<std::optional<std::string>>;

  /**
   * @brief 清理临时媒体文件
   * @param file_path 要删除的文件路径
   */
  static auto cleanup_media_file(const std::string &file_path) -> void;

  /**
   * @brief 获取路径管理器实例
   * @return PathManager 实例的引用
   */
  static auto get_path_manager() -> const PathManager &;

  /**
   * @brief 根据Content-Type判断是否为GIF格式
   * @param content_type MIME类型字符串
   * @return 是否为GIF格式
   */
  static auto is_gif_content_type(const std::string &content_type) -> bool;

  /**
   * @brief 从文件内容检测MIME类型
   * @param content 文件内容的二进制数据
   * @return MIME类型字符串，如 "image/gif", "image/jpeg" 等
   */
  static auto detect_mime_type_from_content(const std::string &content)
      -> std::string;

  /**
   * @brief 从文件内容判断是否为GIF格式
   * @param content 文件内容的二进制数据
   * @return 是否为GIF格式
   */
  static auto is_gif_from_content(const std::string &content) -> bool;
};

// 模板实现
template <typename ConnectionManager>
inline auto MediaProcessor::download_media_file(ConnectionManager *conn_manager,
                                                const std::string &file_url,
                                                const std::string &file_type,
                                                const std::string &filename)
    -> boost::asio::awaitable<std::optional<std::string>> {
  try {
    // 确定文件名
    std::string target_filename = filename;
    if (target_filename.empty()) {
      auto [url, extracted_filename] = get_qq_file_info(file_url, file_type);
      target_filename = extracted_filename;
    }

    // 为了避免文件名冲突，添加时间戳
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count();

    size_t dot_pos = target_filename.find_last_of('.');
    if (dot_pos != std::string::npos) {
      target_filename = target_filename.substr(0, dot_pos) + "_" +
                        std::to_string(timestamp) +
                        target_filename.substr(dot_pos);
    } else {
      target_filename += "_" + std::to_string(timestamp);
    }

    // 构建完整的本地文件路径
    const auto &path_manager = get_path_manager();
    std::string relative_path = "temp/" + target_filename;
    std::string local_file_path = path_manager.to_host_path(relative_path);

    // 创建临时目录（如果不存在）
    path_manager.ensure_directory("temp");

    // 使用ConnectionManager下载文件
    std::string file_content =
        co_await conn_manager->download_file_content(file_url);

    // 写入到本地文件
    std::ofstream output_file(local_file_path, std::ios::binary);
    if (!output_file) {
      throw std::runtime_error("无法创建本地文件: " + local_file_path);
    }

    output_file.write(file_content.data(), file_content.size());
    output_file.close();

    if (!output_file) {
      throw std::runtime_error("写入文件失败: " + local_file_path);
    }

    co_return local_file_path;

  } catch (const std::exception &e) {
    throw; // 重新抛出异常，让调用者处理
  }
}

} // namespace bridge

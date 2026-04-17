#pragma once

#include <string>

namespace bridge {

/**
 * @brief 主机与容器路径转换管理器
 *
 * 用于处理主机挂载目录和容器内挂载目录之间的路径转换
 * 例如：主机路径 ~/Codes/OBCX/tests/llonebot/bridge_files
 *      容器路径 /root/llonebot/bridge_files
 */
class PathManager {
public:
  /**
   * @brief 构造函数
   * @param host_base 主机端挂载目录（绝对路径）
   * @param container_base 容器端挂载目录（绝对路径）
   */
  PathManager(const std::string &host_base, const std::string &container_base);

  /**
   * @brief 将相对路径转换为主机端绝对路径
   * @param relative_path 相对于挂载目录的路径，如 "temp/file.jpg"
   * @return 主机端完整路径，如
   * "~/Codes/OBCX/tests/llonebot/bridge_files/temp/file.jpg"
   */
  auto to_host_path(const std::string &relative_path) const -> std::string;

  /**
   * @brief 将相对路径转换为容器端绝对路径
   * @param relative_path 相对于挂载目录的路径，如 "temp/file.jpg"
   * @return 容器端完整路径，如 "/root/llonebot/bridge_files/temp/file.jpg"
   */
  auto to_container_path(const std::string &relative_path) const -> std::string;

  /**
   * @brief 获取主机端挂载目录
   * @return 主机端挂载目录绝对路径
   */
  auto get_host_base() const -> const std::string &;

  /**
   * @brief 获取容器端挂载目录
   * @return 容器端挂载目录绝对路径
   */
  auto get_container_base() const -> const std::string &;

  /**
   * @brief 将主机端绝对路径转换为容器端绝对路径
   * @param host_absolute_path 主机端绝对路径
   * @return 容器端绝对路径，如果路径不在挂载目录内则返回原路径
   */
  auto host_to_container_absolute(const std::string &host_absolute_path) const
      -> std::string;

  /**
   * @brief 将容器端绝对路径转换为主机端绝对路径
   * @param container_absolute_path 容器端绝对路径
   * @return 主机端绝对路径，如果路径不在挂载目录内则返回原路径
   */
  auto container_to_host_absolute(
      const std::string &container_absolute_path) const -> std::string;

  /**
   * @brief 确保目录存在（在主机端创建）
   * @param relative_path 相对于挂载目录的路径
   * @return 是否成功创建目录
   */
  auto ensure_directory(const std::string &relative_path) const -> bool;

private:
  std::string host_base_;
  std::string container_base_;

  /**
   * @brief 规范化路径（移除末尾斜杠，解析 . 和 .. 等）
   */
  auto normalize_path(const std::string &path) const -> std::string;
};

} // namespace bridge

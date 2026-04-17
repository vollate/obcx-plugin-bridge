#include "path_manager.hpp"

#include <common/logger.hpp>
#include <filesystem>

namespace bridge {

PathManager::PathManager(const std::string &host_base,
                         const std::string &container_base)
    : host_base_(normalize_path(host_base)),
      container_base_(normalize_path(container_base)) {
  PLUGIN_DEBUG(
      "bridge",
      "PathManager initialized with host_base: '{}', container_base: '{}'",
      host_base_, container_base_);
}

auto PathManager::to_host_path(const std::string &relative_path) const
    -> std::string {
  if (relative_path.empty()) {
    return host_base_;
  }

  std::string normalized_relative = normalize_path(relative_path);

  // 移除开头的斜杠（如果有）
  if (normalized_relative.front() == '/') {
    normalized_relative = normalized_relative.substr(1);
  }

  std::string result = host_base_;
  if (!result.empty() && result.back() != '/' && !normalized_relative.empty()) {
    result += '/';
  }
  result += normalized_relative;

  return result;
}

auto PathManager::to_container_path(const std::string &relative_path) const
    -> std::string {
  if (relative_path.empty()) {
    return container_base_;
  }

  std::string normalized_relative = normalize_path(relative_path);

  // 移除开头的斜杠（如果有）
  if (normalized_relative.front() == '/') {
    normalized_relative = normalized_relative.substr(1);
  }

  std::string result = container_base_;
  if (!result.empty() && result.back() != '/' && !normalized_relative.empty()) {
    result += '/';
  }
  result += normalized_relative;

  return result;
}

auto PathManager::get_host_base() const -> const std::string & {
  return host_base_;
}

auto PathManager::get_container_base() const -> const std::string & {
  return container_base_;
}

auto PathManager::host_to_container_absolute(
    const std::string &host_absolute_path) const -> std::string {
  std::string normalized_host_path = normalize_path(host_absolute_path);

  // 检查路径是否以主机基础路径开头
  if (normalized_host_path.find(host_base_) == 0) {
    // 提取相对路径部分
    std::string relative_part =
        normalized_host_path.substr(host_base_.length());

    // 移除开头的斜杠（如果有）
    if (!relative_part.empty() && relative_part.front() == '/') {
      relative_part = relative_part.substr(1);
    }

    return to_container_path(relative_part);
  }

  PLUGIN_WARN("bridge",
              "Host path '{}' is not within the mounted directory '{}'",
              normalized_host_path, host_base_);
  return normalized_host_path; // 返回原路径
}

auto PathManager::container_to_host_absolute(
    const std::string &container_absolute_path) const -> std::string {
  std::string normalized_container_path =
      normalize_path(container_absolute_path);

  // 检查路径是否以容器基础路径开头
  if (normalized_container_path.find(container_base_) == 0) {
    // 提取相对路径部分
    std::string relative_part =
        normalized_container_path.substr(container_base_.length());

    // 移除开头的斜杠（如果有）
    if (!relative_part.empty() && relative_part.front() == '/') {
      relative_part = relative_part.substr(1);
    }

    return to_host_path(relative_part);
  }

  PLUGIN_WARN("bridge",
              "Container path '{}' is not within the mounted directory '{}'",
              normalized_container_path, container_base_);
  return normalized_container_path; // 返回原路径
}

auto PathManager::ensure_directory(const std::string &relative_path) const
    -> bool {
  try {
    std::string host_path = to_host_path(relative_path);
    std::filesystem::create_directories(host_path);
    PLUGIN_DEBUG("bridge", "Ensured directory exists: {}", host_path);
    return true;
  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge",
                 "Failed to create directory for relative path '{}': {}",
                 relative_path, e.what());
    return false;
  }
}

auto PathManager::normalize_path(const std::string &path) const -> std::string {
  if (path.empty()) {
    return path;
  }

  try {
    std::filesystem::path fs_path(path);
    fs_path = std::filesystem::weakly_canonical(fs_path);
    std::string result = fs_path.string();

    // 移除末尾的斜杠（除非是根路径）
    if (result.length() > 1 && result.back() == '/') {
      result.pop_back();
    }

    return result;
  } catch (const std::exception &e) {
    PLUGIN_WARN("bridge", "Failed to normalize path '{}': {}", path, e.what());
    return path; // 返回原路径
  }
}

} // namespace bridge

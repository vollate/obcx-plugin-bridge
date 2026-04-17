#include "database/manager.hpp"

#include <common/logger.hpp>

namespace storage {

// === 消息映射表 DELETE 操作 ===

auto DatabaseManager::delete_message_mapping(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        DELETE FROM message_mappings
        WHERE source_platform = ? AND source_message_id = ? AND target_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare delete statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, source_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, source_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, target_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_DONE) {
    PLUGIN_DEBUG("bridge", "消息映射删除成功: {}:{} -> {}", source_platform,
                 source_message_id, target_platform);
    return true;
  } else {
    PLUGIN_ERROR("bridge", "Failed to delete message mapping: {}",
                 sqlite3_errmsg(db_));
    return false;
  }
}

// === QQ表情包映射表 DELETE 操作 ===

auto DatabaseManager::cleanup_old_image_type_cache(int max_age_days) -> int {
  std::lock_guard lock(db_mutex_);

  if (!db_) {
    PLUGIN_ERROR("bridge", "数据库连接未初始化");
    return -1;
  }

  try {
    // 计算截止时间戳（max_age_days 天前）
    auto cutoff_time = std::chrono::system_clock::now() -
                       std::chrono::hours(24 * max_age_days);
    int64_t cutoff_timestamp = time_point_to_timestamp(cutoff_time);

    // 删除过期的缓存记录（以last_used_at为准）
    const char *sql = R"(
      DELETE FROM qq_sticker_mapping
      WHERE last_used_at IS NOT NULL
      AND last_used_at < ?
    )";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      PLUGIN_ERROR("bridge", "清理缓存SQL准备失败: {}", sqlite3_errmsg(db_));
      return -1;
    }

    sqlite3_bind_int64(stmt, 1, cutoff_timestamp);

    int result = sqlite3_step(stmt);
    int deleted_count = -1;

    if (result == SQLITE_DONE) {
      deleted_count = sqlite3_changes(db_);
      PLUGIN_INFO("bridge", "清理了{}条超过{}天未使用的图片类型缓存记录",
                  deleted_count, max_age_days);
    } else {
      PLUGIN_ERROR("bridge", "清理缓存执行失败: {}", sqlite3_errmsg(db_));
    }

    sqlite3_finalize(stmt);
    return deleted_count;

  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge", "清理图片类型缓存异常: {}", e.what());
    return -1;
  }
}

// === 消息重试队列表 DELETE 操作 ===

auto DatabaseManager::remove_message_retry(const std::string &source_platform,
                                           const std::string &source_message_id,
                                           const std::string &target_platform)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        DELETE FROM message_retry_queue
        WHERE source_platform = ? AND source_message_id = ? AND target_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare remove message retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, source_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, source_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, target_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to remove message retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 媒体下载重试队列表 DELETE 操作 ===

auto DatabaseManager::remove_media_download_retry(const std::string &platform,
                                                  const std::string &file_id)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        DELETE FROM media_download_retry_queue
        WHERE platform = ? AND file_id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare remove media download retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, file_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to remove media download retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

} // namespace storage

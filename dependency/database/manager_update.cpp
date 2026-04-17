#include "database/manager.hpp"

#include <common/logger.hpp>

namespace storage {

// === 消息表 UPDATE 操作 ===

auto DatabaseManager::update_message_forwarding(
    const std::string &platform, const std::string &message_id,
    const std::string &forwarded_to_platform,
    const std::string &forwarded_message_id) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE messages
        SET forwarded_to_platform = ?, forwarded_message_id = ?
        WHERE platform = ? AND message_id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, forwarded_to_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, forwarded_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, message_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update message forwarding: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Message forwarding updated: {}:{} -> {}:{}", platform,
               message_id, forwarded_to_platform, forwarded_message_id);
  return true;
}

// === 消息映射表 UPDATE 操作 ===

auto DatabaseManager::update_message_mapping(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform,
    const std::string &new_target_message_id) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE message_mappings
        SET target_message_id = ?, created_at = datetime('now')
        WHERE source_platform = ? AND source_message_id = ? AND target_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare update statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, new_target_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, source_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, source_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, target_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_DONE) {
    PLUGIN_DEBUG("bridge", "消息映射更新成功: {}:{} -> {}:{}", source_platform,
                 source_message_id, target_platform, new_target_message_id);
    return true;
  } else {
    PLUGIN_ERROR("bridge", "Failed to update message mapping: {}",
                 sqlite3_errmsg(db_));
    return false;
  }
}

// === 表情包缓存表 UPDATE 操作 ===

auto DatabaseManager::update_sticker_last_used(const std::string &platform,
                                               const std::string &sticker_hash)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE sticker_cache
        SET last_used_at = ?
        WHERE platform = ? AND sticker_hash = ?
    )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update sticker last used statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  auto now = std::chrono::system_clock::now();
  sqlite3_bind_int64(stmt, 1, time_point_to_timestamp(now));
  sqlite3_bind_text(stmt, 2, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, sticker_hash.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update sticker last used: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

auto DatabaseManager::update_sticker_conversion(
    const std::string &platform, const std::string &sticker_hash,
    const std::string &conversion_status,
    const std::optional<std::string> &converted_file_path) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE sticker_cache
        SET conversion_status = ?, converted_file_path = ?
        WHERE platform = ? AND sticker_hash = ?
    )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update sticker conversion statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, conversion_status.c_str(), -1, SQLITE_STATIC);

  if (converted_file_path.has_value()) {
    sqlite3_bind_text(stmt, 2, converted_file_path->c_str(), -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 2);
  }

  sqlite3_bind_text(stmt, 3, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, sticker_hash.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update sticker conversion: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === QQ表情包映射表 UPDATE 操作 ===

auto DatabaseManager::update_qq_sticker_last_used(
    const std::string &qq_sticker_hash) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
    UPDATE qq_sticker_mapping
    SET last_used_at = ?
    WHERE qq_sticker_hash = ?
  )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update qq sticker last used statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  auto now = std::chrono::system_clock::now();
  sqlite3_bind_int64(stmt, 1, time_point_to_timestamp(now));
  sqlite3_bind_text(stmt, 2, qq_sticker_hash.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update qq sticker last used: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 消息重试队列表 UPDATE 操作 ===

auto DatabaseManager::update_message_retry(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform, int retry_count,
    const std::chrono::system_clock::time_point &next_retry_at,
    const std::string &failure_reason) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE message_retry_queue
        SET retry_count = ?, next_retry_at = ?, failure_reason = ?, last_attempt_at = ?
        WHERE source_platform = ? AND source_message_id = ? AND target_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update message retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int(stmt, 1, retry_count);
  sqlite3_bind_int64(stmt, 2, time_point_to_timestamp(next_retry_at));
  sqlite3_bind_text(stmt, 3, failure_reason.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4,
                     time_point_to_timestamp(std::chrono::system_clock::now()));
  sqlite3_bind_text(stmt, 5, source_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, source_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, target_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update message retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 媒体下载重试队列表 UPDATE 操作 ===

auto DatabaseManager::update_media_download_retry(
    const std::string &platform, const std::string &file_id, int retry_count,
    const std::chrono::system_clock::time_point &next_retry_at,
    const std::string &failure_reason, bool use_proxy) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        UPDATE media_download_retry_queue
        SET retry_count = ?, next_retry_at = ?, failure_reason = ?, use_proxy = ?, last_attempt_at = ?
        WHERE platform = ? AND file_id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update media download retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int(stmt, 1, retry_count);
  sqlite3_bind_int64(stmt, 2, time_point_to_timestamp(next_retry_at));
  sqlite3_bind_text(stmt, 3, failure_reason.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 4, use_proxy ? 1 : 0);
  sqlite3_bind_int64(stmt, 5,
                     time_point_to_timestamp(std::chrono::system_clock::now()));
  sqlite3_bind_text(stmt, 6, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, file_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update media download retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 平台心跳表 UPDATE 操作 ===

auto DatabaseManager::update_platform_heartbeat(
    const std::string &platform,
    const std::chrono::system_clock::time_point &heartbeat_time) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        INSERT OR REPLACE INTO platform_heartbeats
        (platform, last_heartbeat_at, updated_at)
        VALUES (?, ?, strftime('%s','now'));
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare update platform heartbeat statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  auto heartbeat_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                 heartbeat_time.time_since_epoch())
                                 .count();

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, heartbeat_timestamp);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to update platform heartbeat: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Platform heartbeat updated: {}", platform);
  return true;
}

} // namespace storage

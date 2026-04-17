#include "database/manager.hpp"

#include <chrono>
#include <common/logger.hpp>
#include <iomanip>
#include <optional>
#include <sstream>

namespace storage {

// === 消息表 SELECT 操作 ===

auto DatabaseManager::get_message(const std::string &platform,
                                  const std::string &message_id)
    -> std::optional<MessageInfo> {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        SELECT platform, message_id, group_id, user_id, content, raw_message, message_type,
               timestamp, reply_to_message_id, forwarded_to_platform, forwarded_message_id, created_at
        FROM messages WHERE platform = ? AND message_id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, message_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    MessageInfo msg_info;
    msg_info.platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    msg_info.message_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    msg_info.group_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    msg_info.user_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    msg_info.content =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
    msg_info.raw_message =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    msg_info.message_type =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
    msg_info.timestamp = timestamp_to_time_point(sqlite3_column_int64(stmt, 7));

    const char *reply_to =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
    if (reply_to) {
      msg_info.reply_to_message_id = reply_to;
    }

    const char *forwarded_to =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9));
    if (forwarded_to) {
      msg_info.forwarded_to_platform = forwarded_to;
    }

    const char *forwarded_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 10));
    if (forwarded_id) {
      msg_info.forwarded_message_id = forwarded_id;
    }

    sqlite3_finalize(stmt);
    return msg_info;
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

// === 用户表 SELECT 操作 ===

auto DatabaseManager::get_user(const std::string &platform,
                               const std::string &user_id,
                               const std::string &group_id)
    -> std::optional<UserInfo> {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        SELECT platform, user_id, group_id, username, nickname, title, first_name, last_name, last_updated
        FROM users WHERE platform = ? AND user_id = ? AND group_id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, group_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    UserInfo user_info;
    user_info.platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    user_info.user_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    user_info.group_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));

    const char *username =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    if (username)
      user_info.username = username;

    const char *nickname =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
    if (nickname)
      user_info.nickname = nickname;

    const char *title =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    if (title)
      user_info.title = title;

    const char *first_name =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
    if (first_name)
      user_info.first_name = first_name;

    const char *last_name =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
    if (last_name)
      user_info.last_name = last_name;

    // 读取 last_updated (DATETIME 格式，需要解析)
    const char *last_updated_str =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
    if (last_updated_str) {
      // SQLite CURRENT_TIMESTAMP 是 UTC 时间，格式: "YYYY-MM-DD HH:MM:SS"
      std::tm tm = {};
      std::istringstream ss(last_updated_str);
      ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
      if (!ss.fail()) {
        // 使用 timegm 将 UTC 时间转换为 time_t（而不是 mktime 的本地时间）
        user_info.last_updated =
            std::chrono::system_clock::from_time_t(timegm(&tm));
      }
    }

    sqlite3_finalize(stmt);
    return user_info;
  }
  sqlite3_finalize(stmt);
  return std::nullopt;
}

auto DatabaseManager::query_user_display_name(const std::string &platform,
                                              const std::string &user_id,
                                              const std::string &group_id)
    -> std::optional<std::string> {
  constexpr auto kUserInfoExpiration = std::chrono::minutes(10);

  // Telegram 用户始终使用空 group_id 查询（存储时就是空的）
  std::string query_group_id = (platform == "telegram") ? "" : group_id;
  auto user_info = get_user(platform, user_id, query_group_id);

  if (!user_info.has_value()) {
    PLUGIN_WARN(
        "bridge_db",
        "Database entry not found find for platform: {}, group_id {}, id: {}",
        platform, group_id, user_id);
    // ensure_user_exists(platform, user_id, group_id);
    return std::nullopt;
  }

  const auto &user = user_info.value();

  // 检查用户信息是否过期
  auto now = std::chrono::system_clock::now();
  if (now - user.last_updated > kUserInfoExpiration) {
    PLUGIN_DEBUG(
        "bridge_db",
        "User info expired for platform: {}, id :{}, triggering refresh",
        platform, user_id);
    return std::nullopt;
  }

  // Telegram用户优先显示真实姓名而不是username
  if (platform == "telegram") {
    // 对于Telegram：优先级为 昵称 > 名字+姓氏 > 用户名
    if (!user.nickname.empty()) {
      return user.nickname;
    }

    if (!user.first_name.empty()) {
      std::string display_name = user.first_name;
      if (!user.last_name.empty()) {
        display_name += " " + user.last_name;
      }
      return display_name;
    }

    if (!user.username.empty()) {
      return user.username;
    }
  } else if (platform == "qq") {
    // QQ平台：nickname字段已经存储了最优先的显示名称（群名片 > 群头衔 >
    // 一般昵称） 如果nickname为空，则回退到其他字段
    if (!user.nickname.empty()) {
      return user.nickname;
    }

    if (!user.username.empty()) {
      return user.username;
    }

    if (!user.first_name.empty()) {
      std::string display_name = user.first_name;
      if (!user.last_name.empty()) {
        display_name += " " + user.last_name;
      }
      return display_name;
    }
  } else {
    // 其他平台：优先级为 昵称 > 用户名 > 名字+姓氏
    if (!user.nickname.empty()) {
      return user.nickname;
    }

    if (!user.username.empty()) {
      return user.username;
    }

    if (!user.first_name.empty()) {
      std::string display_name = user.first_name;
      if (!user.last_name.empty()) {
        display_name += " " + user.last_name;
      }
      return display_name;
    }
  }

  // 用户记录存在但所有名称字段都为空
  return std::nullopt;
}

// === 消息映射表 SELECT 操作 ===

auto DatabaseManager::get_target_message_id(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> std::optional<std::string> {
  std::lock_guard lock(db_mutex_);

  // 验证参数不为空
  if (source_message_id.empty()) {
    PLUGIN_DEBUG("bridge", "Empty source message ID for query: {}:{} -> {}",
                 source_platform, source_message_id, target_platform);
    return std::nullopt;
  }

  PLUGIN_DEBUG("bridge", "Querying target message ID: {}:{} -> {}",
               source_platform, source_message_id, target_platform);

  const std::string sql = R"(
        SELECT target_message_id FROM message_mappings
        WHERE source_platform = ? AND source_message_id = ? AND target_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, source_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, source_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, target_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    std::string result =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    PLUGIN_DEBUG("bridge", "Found target message ID: {}", result);
    return result;
  }

  sqlite3_finalize(stmt);
  PLUGIN_DEBUG("bridge", "No target message ID found");
  return std::nullopt;
}

auto DatabaseManager::get_source_message_id(
    const std::string &target_platform, const std::string &target_message_id,
    const std::string &source_platform) -> std::optional<std::string> {
  std::lock_guard lock(db_mutex_);

  // 验证参数不为空
  if (target_message_id.empty()) {
    PLUGIN_DEBUG("bridge", "Empty target message ID for query: {}:{} <- {}",
                 target_platform, target_message_id, source_platform);
    return std::nullopt;
  }

  PLUGIN_DEBUG("bridge", "Querying source message ID: {}:{} <- {}",
               target_platform, target_message_id, source_platform);

  const std::string sql = R"(
        SELECT source_message_id FROM message_mappings
        WHERE target_platform = ? AND target_message_id = ? AND source_platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, target_platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, target_message_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, source_platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    std::string result =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    PLUGIN_DEBUG("bridge", "Found source message ID: {}", result);
    return result;
  }

  sqlite3_finalize(stmt);
  PLUGIN_DEBUG("bridge", "No source message ID found");
  return std::nullopt;
}

// === 表情包缓存表 SELECT 操作 ===

auto DatabaseManager::get_sticker_cache(const std::string &platform,
                                        const std::string &sticker_hash)
    -> std::optional<StickerCacheInfo> {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        SELECT platform, sticker_id, sticker_hash, original_name, file_type, mime_type,
               original_file_path, converted_file_path, container_path, file_size,
               conversion_status, created_at, last_used_at
        FROM sticker_cache
        WHERE platform = ? AND sticker_hash = ?
    )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare get sticker cache statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, sticker_hash.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  StickerCacheInfo cache_info;
  int idx = 0;

  cache_info.platform =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  cache_info.sticker_id =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  cache_info.sticker_hash =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));

  const char *original_name =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  if (original_name) {
    cache_info.original_name = original_name;
  }

  cache_info.file_type =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));

  const char *mime_type =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  if (mime_type) {
    cache_info.mime_type = mime_type;
  }

  cache_info.original_file_path =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));

  const char *converted_file_path =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  if (converted_file_path) {
    cache_info.converted_file_path = converted_file_path;
  }

  cache_info.container_path =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));

  if (sqlite3_column_type(stmt, idx) != SQLITE_NULL) {
    cache_info.file_size = sqlite3_column_int64(stmt, idx);
  }
  idx++;

  cache_info.conversion_status =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, idx++));
  cache_info.created_at =
      timestamp_to_time_point(sqlite3_column_int64(stmt, idx++));
  cache_info.last_used_at =
      timestamp_to_time_point(sqlite3_column_int64(stmt, idx++));

  sqlite3_finalize(stmt);

  PLUGIN_DEBUG("bridge", "Sticker cache found: {} - {}", platform,
               sticker_hash);
  return cache_info;
}

// === QQ表情包映射表 SELECT 操作 ===

auto DatabaseManager::get_qq_sticker_mapping(const std::string &qq_sticker_hash)
    -> std::optional<QQStickerMapping> {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
    SELECT qq_sticker_hash, telegram_file_id, file_type, created_at, last_used_at, is_gif, content_type, last_checked_at
    FROM qq_sticker_mapping
    WHERE qq_sticker_hash = ?
  )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare get qq sticker mapping statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, qq_sticker_hash.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    QQStickerMapping mapping;
    mapping.qq_sticker_hash =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    mapping.telegram_file_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    mapping.file_type =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    mapping.created_at = timestamp_to_time_point(sqlite3_column_int64(stmt, 3));
    mapping.last_used_at =
        timestamp_to_time_point(sqlite3_column_int64(stmt, 4));

    // 处理可选字段
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
      mapping.is_gif = sqlite3_column_int(stmt, 5) == 1;
    }

    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
      mapping.content_type =
          reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
    }

    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
      mapping.last_checked_at =
          timestamp_to_time_point(sqlite3_column_int64(stmt, 7));
    }

    sqlite3_finalize(stmt);
    return mapping;
  } else if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to get qq sticker mapping: {}",
                 sqlite3_errmsg(db_));
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

auto DatabaseManager::get_cache_statistics() -> std::string {
  std::lock_guard lock(db_mutex_);

  if (!db_) {
    return "数据库连接未初始化";
  }

  std::ostringstream stats;

  try {
    // 统计总记录数
    const char *count_sql = "SELECT COUNT(*) FROM qq_sticker_mapping";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db_, count_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        int total_count = sqlite3_column_int(stmt, 0);
        stats << "总缓存记录数: " << total_count << "\n";
      }
      sqlite3_finalize(stmt);
    }

    // 统计有图片类型信息的记录数
    const char *typed_sql = R"(
      SELECT COUNT(*) FROM qq_sticker_mapping
      WHERE is_gif IS NOT NULL
    )";

    if (sqlite3_prepare_v2(db_, typed_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        int typed_count = sqlite3_column_int(stmt, 0);
        stats << "已检测类型的记录数: " << typed_count << "\n";
      }
      sqlite3_finalize(stmt);
    }

    // 统计GIF和非GIF的分布
    const char *gif_sql = R"(
      SELECT
        SUM(CASE WHEN is_gif = 1 THEN 1 ELSE 0 END) as gif_count,
        SUM(CASE WHEN is_gif = 0 THEN 1 ELSE 0 END) as static_count
      FROM qq_sticker_mapping
      WHERE is_gif IS NOT NULL
    )";

    if (sqlite3_prepare_v2(db_, gif_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        int gif_count = sqlite3_column_int(stmt, 0);
        int static_count = sqlite3_column_int(stmt, 1);
        stats << "GIF图片数: " << gif_count << "\n";
        stats << "静态图片数: " << static_count << "\n";
      }
      sqlite3_finalize(stmt);
    }

    // 统计最近检测时间
    const char *recent_sql = R"(
      SELECT COUNT(*) FROM qq_sticker_mapping
      WHERE last_checked_at IS NOT NULL
      AND last_checked_at > ?
    )";

    auto recent_time =
        std::chrono::system_clock::now() - std::chrono::hours(24);
    int64_t recent_timestamp = time_point_to_timestamp(recent_time);

    if (sqlite3_prepare_v2(db_, recent_sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, recent_timestamp);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        int recent_count = sqlite3_column_int(stmt, 0);
        stats << "24小时内检测的记录数: " << recent_count << "\n";
      }
      sqlite3_finalize(stmt);
    }

  } catch (const std::exception &e) {
    stats << "获取缓存统计异常: " << e.what() << "\n";
  }

  return stats.str();
}

// === 消息重试队列表 SELECT 操作 ===

auto DatabaseManager::get_pending_message_retries(int limit)
    -> std::vector<MessageRetryInfo> {
  std::lock_guard lock(db_mutex_);
  std::vector<MessageRetryInfo> retries;

  const std::string sql = R"(
        SELECT source_platform, target_platform, source_message_id, message_content,
               group_id, source_group_id, target_topic_id, retry_count, max_retry_count,
               failure_reason, retry_type, next_retry_at, created_at, last_attempt_at
        FROM message_retry_queue
        WHERE next_retry_at <= ? AND retry_count < max_retry_count
        ORDER BY next_retry_at ASC
        LIMIT ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare get pending message retries statement: {}",
                 sqlite3_errmsg(db_));
    return retries;
  }

  int64_t current_time =
      time_point_to_timestamp(std::chrono::system_clock::now());
  sqlite3_bind_int64(stmt, 1, current_time);
  sqlite3_bind_int(stmt, 2, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MessageRetryInfo info;
    info.source_platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    info.target_platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    info.source_message_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    info.message_content =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    info.group_id =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));

    const char *source_group_id_ptr =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    info.source_group_id = source_group_id_ptr ? source_group_id_ptr : "";

    info.target_topic_id = sqlite3_column_int64(stmt, 6);
    info.retry_count = sqlite3_column_int(stmt, 7);
    info.max_retry_count = sqlite3_column_int(stmt, 8);

    const char *failure_reason =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9));
    info.failure_reason = failure_reason ? failure_reason : "";

    info.retry_type =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 10));
    info.next_retry_at =
        timestamp_to_time_point(sqlite3_column_int64(stmt, 11));
    info.created_at = timestamp_to_time_point(sqlite3_column_int64(stmt, 12));
    info.last_attempt_at =
        timestamp_to_time_point(sqlite3_column_int64(stmt, 13));

    retries.push_back(info);
  }

  sqlite3_finalize(stmt);
  return retries;
}

// === 媒体下载重试队列表 SELECT 操作 ===

auto DatabaseManager::get_pending_media_download_retries(int limit)
    -> std::vector<MediaDownloadRetryInfo> {
  std::lock_guard lock(db_mutex_);
  std::vector<MediaDownloadRetryInfo> retries;

  const std::string sql = R"(
        SELECT platform, file_id, file_type, download_url, local_path,
               retry_count, max_retry_count, failure_reason, use_proxy,
               next_retry_at, created_at, last_attempt_at
        FROM media_download_retry_queue
        WHERE next_retry_at <= ? AND retry_count < max_retry_count
        ORDER BY next_retry_at ASC
        LIMIT ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR(
        "bridge",
        "Failed to prepare get pending media download retries statement: {}",
        sqlite3_errmsg(db_));
    return retries;
  }

  int64_t current_time =
      time_point_to_timestamp(std::chrono::system_clock::now());
  sqlite3_bind_int64(stmt, 1, current_time);
  sqlite3_bind_int(stmt, 2, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MediaDownloadRetryInfo info;
    info.platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    info.file_id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    info.file_type =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    info.download_url =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    info.local_path =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
    info.retry_count = sqlite3_column_int(stmt, 5);
    info.max_retry_count = sqlite3_column_int(stmt, 6);

    const char *failure_reason =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
    info.failure_reason = failure_reason ? failure_reason : "";

    info.use_proxy = sqlite3_column_int(stmt, 8) != 0;
    info.next_retry_at = timestamp_to_time_point(sqlite3_column_int64(stmt, 9));
    info.created_at = timestamp_to_time_point(sqlite3_column_int64(stmt, 10));
    info.last_attempt_at =
        timestamp_to_time_point(sqlite3_column_int64(stmt, 11));

    retries.push_back(info);
  }

  sqlite3_finalize(stmt);
  return retries;
}

// === 平台心跳表 SELECT 操作 ===

auto DatabaseManager::get_platform_heartbeat(const std::string &platform)
    -> std::optional<PlatformHeartbeatInfo> {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        SELECT platform, last_heartbeat_at, updated_at
        FROM platform_heartbeats
        WHERE platform = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare get platform heartbeat statement: {}",
                 sqlite3_errmsg(db_));
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    PlatformHeartbeatInfo info;
    info.platform =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));

    int64_t heartbeat_timestamp = sqlite3_column_int64(stmt, 1);
    info.last_heartbeat_at =
        std::chrono::system_clock::from_time_t(heartbeat_timestamp);

    int64_t updated_timestamp = sqlite3_column_int64(stmt, 2);
    info.updated_at = std::chrono::system_clock::from_time_t(updated_timestamp);

    sqlite3_finalize(stmt);
    return info;
  }

  sqlite3_finalize(stmt);
  return std::nullopt;
}

} // namespace storage

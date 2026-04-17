#include "database/manager.hpp"

#include <common/logger.hpp>
#include <nlohmann/json.hpp>

namespace storage {

// === 消息表 INSERT 操作 ===

auto DatabaseManager::save_message(const MessageInfo &message_info) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        INSERT OR REPLACE INTO messages
        (platform, message_id, group_id, user_id, content, raw_message, message_type,
         timestamp, reply_to_message_id, forwarded_to_platform, forwarded_message_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  // 绑定参数
  sqlite3_bind_text(stmt, 1, message_info.platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, message_info.message_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, message_info.group_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, message_info.user_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, message_info.content.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, message_info.raw_message.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, message_info.message_type.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 8, time_point_to_timestamp(message_info.timestamp));

  if (message_info.reply_to_message_id.has_value()) {
    sqlite3_bind_text(stmt, 9, message_info.reply_to_message_id->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 9);
  }

  if (message_info.forwarded_to_platform.has_value()) {
    sqlite3_bind_text(stmt, 10, message_info.forwarded_to_platform->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 10);
  }

  if (message_info.forwarded_message_id.has_value()) {
    sqlite3_bind_text(stmt, 11, message_info.forwarded_message_id->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 11);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to insert message: {}", sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Message saved: {}:{}", message_info.platform,
               message_info.message_id);
  return true;
}

auto DatabaseManager::save_message_from_event(
    const obcx::common::MessageEvent &event, const std::string &platform)
    -> bool {
  MessageInfo msg_info;
  msg_info.platform = platform;
  msg_info.message_id = event.message_id;
  msg_info.group_id = event.group_id.value_or("");
  msg_info.user_id = event.user_id;
  msg_info.content = event.raw_message;
  msg_info.timestamp = event.time;

  // 将消息序列化为JSON存储
  nlohmann::json raw_json;
  raw_json["type"] = event.type;
  raw_json["post_type"] = event.post_type;
  raw_json["message_type"] = event.message_type;
  raw_json["raw_message"] = event.raw_message;
  raw_json["message"] = nlohmann::json::array();

  for (const auto &segment : event.message) {
    nlohmann::json segment_json;
    segment_json["type"] = segment.type;
    segment_json["data"] = segment.data;
    raw_json["message"].push_back(segment_json);
  }

  msg_info.raw_message = raw_json.dump();

  // 确定消息类型
  if (!event.message.empty()) {
    msg_info.message_type = event.message[0].type;
  }

  // 检查是否有回复消息
  for (const auto &segment : event.message) {
    if (segment.type == "reply" && segment.data.contains("id")) {
      msg_info.reply_to_message_id = segment.data["id"];
      break;
    }
  }

  return save_message(msg_info);
}

// === 用户表 INSERT 操作 ===

auto DatabaseManager::save_or_update_user(const UserInfo &user_info,
                                          bool force_update) -> bool {
  std::lock_guard lock(db_mutex_);

  std::string sql;
  if (force_update) {
    // 强制更新：无论字段是否变化，都更新所有字段和last_updated
    sql = R"(
        INSERT INTO users
        (platform, user_id, group_id, username, nickname, title, first_name, last_name, last_updated)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(platform, user_id, group_id) DO UPDATE SET
            username = excluded.username,
            nickname = excluded.nickname,
            title = excluded.title,
            first_name = excluded.first_name,
            last_name = excluded.last_name,
            last_updated = CURRENT_TIMESTAMP;
    )";
  } else {
    // 懒更新：只有在关键字段变化时才更新，避免无效 I/O
    // last_updated 只在有实际变化时才更新
    // 使用 IFNULL 将 NULL 和空字符串视为相同，避免 NULL vs '' 导致的无效更新
    sql = R"(
        INSERT INTO users
        (platform, user_id, group_id, username, nickname, title, first_name, last_name, last_updated)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(platform, user_id, group_id) DO UPDATE SET
            username = excluded.username,
            nickname = excluded.nickname,
            title = excluded.title,
            first_name = excluded.first_name,
            last_name = excluded.last_name,
            last_updated = CURRENT_TIMESTAMP
        WHERE
            IFNULL(users.username, '') != IFNULL(excluded.username, '') OR
            IFNULL(users.nickname, '') != IFNULL(excluded.nickname, '') OR
            IFNULL(users.title, '') != IFNULL(excluded.title, '') OR
            IFNULL(users.first_name, '') != IFNULL(excluded.first_name, '') OR
            IFNULL(users.last_name, '') != IFNULL(excluded.last_name, '');
    )";
  }

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, user_info.platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, user_info.user_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, user_info.group_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, user_info.username.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, user_info.nickname.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, user_info.title.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 7, user_info.first_name.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 8, user_info.last_name.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_OK && rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to save user: {} (rc={})",
                 sqlite3_errmsg(db_), rc);
    return false;
  }

  int changes = sqlite3_changes(db_);
  PLUGIN_DEBUG("bridge",
               "User save attempt: {}:{}:{} changes={} nickname='{}' "
               "username='{}' title='{}'",
               user_info.platform, user_info.user_id, user_info.group_id,
               changes, user_info.nickname, user_info.username,
               user_info.title);
  return true;
}

auto DatabaseManager::save_user_from_event(
    const obcx::common::MessageEvent &event, const std::string &platform)
    -> bool {
  UserInfo user_info;
  user_info.platform = platform;
  user_info.user_id = event.user_id;
  // 只有QQ用户使用群组特定的昵称，Telegram用户始终使用空的group_id
  user_info.group_id = (platform == "qq") ? event.group_id.value_or("") : "";
  user_info.last_updated = std::chrono::system_clock::now();

  // 尝试从event.data中提取更多用户信息
  if (event.data.is_object()) {
    if (platform == "telegram") {
      // 对于Telegram，从from字段提取用户信息
      if (event.data.contains("from") && event.data["from"].is_object()) {
        auto from = event.data["from"];
        if (from.contains("username") && from["username"].is_string()) {
          user_info.username = from["username"];
        }
        if (from.contains("first_name") && from["first_name"].is_string()) {
          user_info.first_name = from["first_name"];
        }
        if (from.contains("last_name") && from["last_name"].is_string()) {
          user_info.last_name = from["last_name"];
        }
      }
    } else if (platform == "qq") {
      // 对于QQ，从sender字段提取用户信息
      if (event.data.contains("sender") && event.data["sender"].is_object()) {
        auto sender = event.data["sender"];
        if (sender.contains("nickname") && sender["nickname"].is_string()) {
          user_info.nickname = sender["nickname"];
        }
        if (sender.contains("card") && sender["card"].is_string()) {
          std::string card = sender["card"];
          if (!card.empty()) {
            user_info.nickname = card; // QQ群名片作为昵称
          }
        }
      }
    }
  }

  // 如果没有提取到任何有意义的用户信息，仍然确保用户记录存在（INSERT OR
  // IGNORE） 这样 last_updated 可以被设置，expiry 机制才能正常工作
  if (user_info.nickname.empty() && user_info.username.empty() &&
      user_info.first_name.empty() && user_info.last_name.empty()) {
    return true; // 静默跳过，不是错误

    // return ensure_user_exists(user_info.platform, user_info.user_id,
    // user_info.group_id);
  }

  return save_or_update_user(user_info);
}

auto DatabaseManager::ensure_user_exists(const std::string &platform,
                                         const std::string &user_id,
                                         const std::string &group_id) -> bool {
  std::lock_guard lock(db_mutex_);

  // INSERT OR IGNORE：若用户已存在则不做任何修改，保留已有的名称字段
  const std::string sql = R"(
      INSERT OR IGNORE INTO users (platform, user_id, group_id, last_updated)
      VALUES (?, ?, ?, CURRENT_TIMESTAMP);
  )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare ensure_user_exists statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, group_id.c_str(), -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to ensure user exists: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Ensured user exists: {}:{}:{}", platform, user_id,
               group_id);
  return true;
}

// === 消息映射表 INSERT 操作 ===

auto DatabaseManager::add_message_mapping(const MessageMapping &mapping)
    -> bool {
  std::lock_guard lock(db_mutex_);

  // 验证消息ID不为空
  if (mapping.source_message_id.empty() || mapping.target_message_id.empty()) {
    PLUGIN_ERROR(
        "bridge",
        "Invalid message mapping - empty message IDs: source={}, target={}",
        mapping.source_message_id, mapping.target_message_id);
    return false;
  }

  PLUGIN_DEBUG("bridge", "Adding message mapping: {}:{} -> {}:{}",
               mapping.source_platform, mapping.source_message_id,
               mapping.target_platform, mapping.target_message_id);

  const std::string sql = R"(
        INSERT OR REPLACE INTO message_mappings
        (source_platform, source_message_id, target_platform, target_message_id)
        VALUES (?, ?, ?, ?);
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, mapping.source_platform.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, mapping.source_message_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, mapping.target_platform.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, mapping.target_message_id.c_str(), -1,
                    SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to add message mapping: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Message mapping added: {}:{} -> {}:{}",
               mapping.source_platform, mapping.source_message_id,
               mapping.target_platform, mapping.target_message_id);
  return true;
}

// === 表情包缓存表 INSERT 操作 ===

auto DatabaseManager::save_sticker_cache(const StickerCacheInfo &cache_info)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        INSERT OR REPLACE INTO sticker_cache (
            platform, sticker_id, sticker_hash, original_name, file_type, mime_type,
            original_file_path, converted_file_path, container_path, file_size,
            conversion_status, created_at, last_used_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare save sticker cache statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  int idx = 1;
  sqlite3_bind_text(stmt, idx++, cache_info.platform.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, idx++, cache_info.sticker_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, idx++, cache_info.sticker_hash.c_str(), -1,
                    SQLITE_STATIC);

  if (cache_info.original_name.has_value()) {
    sqlite3_bind_text(stmt, idx++, cache_info.original_name->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, idx++);
  }

  sqlite3_bind_text(stmt, idx++, cache_info.file_type.c_str(), -1,
                    SQLITE_STATIC);

  if (cache_info.mime_type.has_value()) {
    sqlite3_bind_text(stmt, idx++, cache_info.mime_type->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, idx++);
  }

  sqlite3_bind_text(stmt, idx++, cache_info.original_file_path.c_str(), -1,
                    SQLITE_STATIC);

  if (cache_info.converted_file_path.has_value()) {
    sqlite3_bind_text(stmt, idx++, cache_info.converted_file_path->c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, idx++);
  }

  sqlite3_bind_text(stmt, idx++, cache_info.container_path.c_str(), -1,
                    SQLITE_STATIC);

  if (cache_info.file_size.has_value()) {
    sqlite3_bind_int64(stmt, idx++, cache_info.file_size.value());
  } else {
    sqlite3_bind_null(stmt, idx++);
  }

  sqlite3_bind_text(stmt, idx++, cache_info.conversion_status.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(stmt, idx++,
                     time_point_to_timestamp(cache_info.created_at));
  sqlite3_bind_int64(stmt, idx++,
                     time_point_to_timestamp(cache_info.last_used_at));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to save sticker cache: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_DEBUG("bridge", "Sticker cache saved successfully: {} - {}",
               cache_info.platform, cache_info.sticker_hash);
  return true;
}

// === QQ表情包映射表 INSERT 操作 ===

auto DatabaseManager::save_qq_sticker_mapping(const QQStickerMapping &mapping)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
    INSERT OR REPLACE INTO qq_sticker_mapping
    (qq_sticker_hash, telegram_file_id, file_type, created_at, last_used_at, is_gif, content_type, last_checked_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  )";

  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare save qq sticker mapping statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, mapping.qq_sticker_hash.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, mapping.telegram_file_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, mapping.file_type.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, time_point_to_timestamp(mapping.created_at));
  sqlite3_bind_int64(stmt, 5, time_point_to_timestamp(mapping.last_used_at));

  // 处理可选字段
  if (mapping.is_gif.has_value()) {
    sqlite3_bind_int(stmt, 6, mapping.is_gif.value() ? 1 : 0);
  } else {
    sqlite3_bind_null(stmt, 6);
  }

  if (mapping.content_type.has_value()) {
    sqlite3_bind_text(stmt, 7, mapping.content_type.value().c_str(), -1,
                      SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 7);
  }

  if (mapping.last_checked_at.has_value()) {
    sqlite3_bind_int64(
        stmt, 8, time_point_to_timestamp(mapping.last_checked_at.value()));
  } else {
    sqlite3_bind_null(stmt, 8);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to save qq sticker mapping: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 消息重试队列表 INSERT 操作 ===

auto DatabaseManager::add_message_retry(const MessageRetryInfo &retry_info)
    -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        INSERT OR REPLACE INTO message_retry_queue
        (source_platform, target_platform, source_message_id, message_content,
         group_id, source_group_id, target_topic_id, retry_count, max_retry_count,
         failure_reason, retry_type, next_retry_at, created_at, last_attempt_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Failed to prepare message retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, retry_info.source_platform.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, retry_info.target_platform.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, retry_info.source_message_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, retry_info.message_content.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, retry_info.group_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, retry_info.source_group_id.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 7, retry_info.target_topic_id);
  sqlite3_bind_int(stmt, 8, retry_info.retry_count);
  sqlite3_bind_int(stmt, 9, retry_info.max_retry_count);
  sqlite3_bind_text(stmt, 10, retry_info.failure_reason.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 11, retry_info.retry_type.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 12,
                     time_point_to_timestamp(retry_info.next_retry_at));
  sqlite3_bind_int64(stmt, 13, time_point_to_timestamp(retry_info.created_at));
  sqlite3_bind_int64(stmt, 14,
                     time_point_to_timestamp(retry_info.last_attempt_at));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to insert message retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

// === 媒体下载重试队列表 INSERT 操作 ===

auto DatabaseManager::add_media_download_retry(
    const MediaDownloadRetryInfo &retry_info) -> bool {
  std::lock_guard lock(db_mutex_);

  const std::string sql = R"(
        INSERT OR REPLACE INTO media_download_retry_queue
        (platform, file_id, file_type, download_url, local_path,
         retry_count, max_retry_count, failure_reason, use_proxy,
         next_retry_at, created_at, last_attempt_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge",
                 "Failed to prepare media download retry statement: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, retry_info.platform.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, retry_info.file_id.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, retry_info.file_type.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, retry_info.download_url.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, retry_info.local_path.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 6, retry_info.retry_count);
  sqlite3_bind_int(stmt, 7, retry_info.max_retry_count);
  sqlite3_bind_text(stmt, 8, retry_info.failure_reason.c_str(), -1,
                    SQLITE_STATIC);
  sqlite3_bind_int(stmt, 9, retry_info.use_proxy ? 1 : 0);
  sqlite3_bind_int64(stmt, 10,
                     time_point_to_timestamp(retry_info.next_retry_at));
  sqlite3_bind_int64(stmt, 11, time_point_to_timestamp(retry_info.created_at));
  sqlite3_bind_int64(stmt, 12,
                     time_point_to_timestamp(retry_info.last_attempt_at));

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    PLUGIN_ERROR("bridge", "Failed to insert media download retry: {}",
                 sqlite3_errmsg(db_));
    return false;
  }

  return true;
}

} // namespace storage

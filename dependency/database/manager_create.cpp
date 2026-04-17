#include "database/manager.hpp"

#include <common/logger.hpp>

namespace storage {

// === 表创建操作 ===

auto DatabaseManager::create_tables() -> bool {
  // 创建消息表
  const std::string create_messages_table = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            message_id TEXT NOT NULL,
            group_id TEXT NOT NULL,
            user_id TEXT NOT NULL,
            content TEXT NOT NULL,
            raw_message TEXT,
            message_type TEXT DEFAULT 'text',
            timestamp INTEGER NOT NULL,
            reply_to_message_id TEXT,
            forwarded_to_platform TEXT,
            forwarded_message_id TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(platform, message_id, group_id)
        );
    )";

  if (!execute_sql(create_messages_table)) {
    return false;
  }

  // 创建用户表
  const std::string create_users_table = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            user_id TEXT NOT NULL,
            group_id TEXT NOT NULL DEFAULT '',
            username TEXT,
            nickname TEXT,
            title TEXT,
            first_name TEXT,
            last_name TEXT,
            last_updated DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(platform, user_id, group_id)
        );
    )";

  if (!execute_sql(create_users_table)) {
    return false;
  }

  // 创建消息ID映射表
  const std::string create_mappings_table = R"(
        CREATE TABLE IF NOT EXISTS message_mappings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_platform TEXT NOT NULL,
            source_message_id TEXT NOT NULL,
            target_platform TEXT NOT NULL,
            target_message_id TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(source_platform, source_message_id, target_platform)
        );
    )";

  if (!execute_sql(create_mappings_table)) {
    return false;
  }

  // 创建表情包缓存表
  const std::string create_sticker_cache_table = R"(
        CREATE TABLE IF NOT EXISTS sticker_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            sticker_id TEXT NOT NULL,
            sticker_hash TEXT NOT NULL,
            original_name TEXT,
            file_type TEXT NOT NULL,
            mime_type TEXT,
            original_file_path TEXT NOT NULL,
            converted_file_path TEXT,
            container_path TEXT NOT NULL,
            file_size INTEGER,
            conversion_status TEXT DEFAULT 'none',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            last_used_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(platform, sticker_hash)
        );
    )";

  if (!execute_sql(create_sticker_cache_table)) {
    return false;
  }

  // QQ表情包映射表
  const std::string create_qq_sticker_mapping_table = R"(
    CREATE TABLE IF NOT EXISTS qq_sticker_mapping (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      qq_sticker_hash TEXT NOT NULL UNIQUE,
      telegram_file_id TEXT NOT NULL,
      file_type TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      last_used_at INTEGER NOT NULL,
      is_gif INTEGER DEFAULT NULL,
      content_type TEXT DEFAULT NULL,
      last_checked_at INTEGER DEFAULT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_qq_sticker_hash ON qq_sticker_mapping(qq_sticker_hash);
  )";

  if (!execute_sql(create_qq_sticker_mapping_table)) {
    return false;
  }

  // 创建消息重试队列表
  const std::string create_message_retry_table = R"(
        CREATE TABLE IF NOT EXISTS message_retry_queue (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_platform TEXT NOT NULL,
            target_platform TEXT NOT NULL,
            source_message_id TEXT NOT NULL,
            message_content TEXT NOT NULL,
            group_id TEXT NOT NULL,
            source_group_id TEXT,
            target_topic_id INTEGER DEFAULT -1,
            retry_count INTEGER NOT NULL DEFAULT 0,
            max_retry_count INTEGER NOT NULL DEFAULT 5,
            failure_reason TEXT,
            retry_type TEXT NOT NULL DEFAULT 'message_send',
            next_retry_at INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            last_attempt_at INTEGER NOT NULL,
            UNIQUE(source_platform, source_message_id, target_platform)
        );
    )";

  if (!execute_sql(create_message_retry_table)) {
    return false;
  }

  // 创建媒体下载重试队列表
  const std::string create_media_retry_table = R"(
        CREATE TABLE IF NOT EXISTS media_download_retry_queue (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            file_id TEXT NOT NULL,
            file_type TEXT NOT NULL,
            download_url TEXT NOT NULL,
            local_path TEXT NOT NULL,
            retry_count INTEGER NOT NULL DEFAULT 0,
            max_retry_count INTEGER NOT NULL DEFAULT 3,
            failure_reason TEXT,
            use_proxy INTEGER NOT NULL DEFAULT 1,
            next_retry_at INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            last_attempt_at INTEGER NOT NULL,
            UNIQUE(platform, file_id)
        );
    )";

  if (!execute_sql(create_media_retry_table)) {
    return false;
  }

  // 创建索引以提高查询性能
  const std::string create_retry_indexes = R"(
        CREATE INDEX IF NOT EXISTS idx_message_retry_next_retry ON message_retry_queue(next_retry_at);
        CREATE INDEX IF NOT EXISTS idx_media_retry_next_retry ON media_download_retry_queue(next_retry_at);
    )";

  if (!execute_sql(create_retry_indexes)) {
    return false;
  }

  // 创建平台心跳表
  const std::string create_heartbeat_table = R"(
        CREATE TABLE IF NOT EXISTS platform_heartbeats (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL UNIQUE,
            last_heartbeat_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
    )";
  if (!execute_sql(create_heartbeat_table)) {
    return false;
  }

  PLUGIN_INFO("bridge", "Database tables created successfully");
  return true;
}

} // namespace storage

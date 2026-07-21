#include "bridge_state_repository.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>

namespace bridge {
namespace {

auto timestamp_ms(const std::chrono::system_clock::time_point &time)
    -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             time.time_since_epoch())
      .count();
}

auto time_point_from_ms(const std::int64_t timestamp)
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point{
      std::chrono::milliseconds{timestamp}};
}

auto db_string(const obcx::core::DbRow &row, const std::string &column)
    -> std::string {
  return std::get<std::string>(row.at(column));
}

auto db_int64(const obcx::core::DbRow &row, const std::string &column)
    -> std::int64_t {
  return std::get<std::int64_t>(row.at(column));
}

auto optional_string(const std::optional<std::string> &value)
    -> obcx::core::DbValue {
  if (!value.has_value()) {
    return nullptr;
  }
  return *value;
}

auto optional_int64(const std::optional<std::int64_t> &value)
    -> obcx::core::DbValue {
  if (!value.has_value()) {
    return nullptr;
  }
  return *value;
}

auto optional_bool(const std::optional<bool> &value) -> obcx::core::DbValue {
  if (!value.has_value()) {
    return nullptr;
  }
  return static_cast<std::int64_t>(*value ? 1 : 0);
}

auto optional_time_ms(
    const std::optional<std::chrono::system_clock::time_point> &value)
    -> obcx::core::DbValue {
  if (!value.has_value()) {
    return nullptr;
  }
  return timestamp_ms(*value);
}

auto db_optional_string(const obcx::core::DbRow &row, const std::string &column)
    -> std::optional<std::string> {
  const auto &value = row.at(column);
  if (std::holds_alternative<std::nullptr_t>(value)) {
    return std::nullopt;
  }
  return std::get<std::string>(value);
}

auto db_optional_int64(const obcx::core::DbRow &row, const std::string &column)
    -> std::optional<std::int64_t> {
  const auto &value = row.at(column);
  if (std::holds_alternative<std::nullptr_t>(value)) {
    return std::nullopt;
  }
  return std::get<std::int64_t>(value);
}

auto db_optional_bool(const obcx::core::DbRow &row, const std::string &column)
    -> std::optional<bool> {
  const auto value = db_optional_int64(row, column);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return *value != 0;
}

auto db_optional_time(const obcx::core::DbRow &row, const std::string &column)
    -> std::optional<std::chrono::system_clock::time_point> {
  const auto value = db_optional_int64(row, column);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return time_point_from_ms(*value);
}

} // namespace

BridgeStateRepository::BridgeStateRepository(obcx::core::DbManager &db_manager,
                                             std::string db_instance,
                                             std::string db_namespace)
    : db_manager_(db_manager), db_instance_(std::move(db_instance)),
      db_namespace_(std::move(db_namespace)) {
  if (db_instance_.empty()) {
    throw std::invalid_argument("Bridge DB instance cannot be empty");
  }
  if (db_namespace_.empty()) {
    throw std::invalid_argument("Bridge DB namespace cannot be empty");
  }
}

void BridgeStateRepository::initialize_schema() {
  db_manager_.with_migration_lock(db_instance_, db_namespace_,
                                  [](obcx::core::IDbConnection &connection) {
                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_message_mappings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_platform TEXT NOT NULL,
            source_message_id TEXT NOT NULL,
            target_platform TEXT NOT NULL,
            target_message_id TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            UNIQUE(source_platform, source_message_id, target_platform)
          );
        )");

                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_message_retry_queue (
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
        )");

                                    connection.execute(R"(
          CREATE INDEX IF NOT EXISTS idx_bridge_message_retry_next_retry
          ON bridge_message_retry_queue(next_retry_at);
        )");

                                    connection.execute(R"(
	          CREATE TABLE IF NOT EXISTS bridge_media_group_mappings (
	            id INTEGER PRIMARY KEY AUTOINCREMENT,
            source_platform TEXT NOT NULL,
            media_group_id TEXT NOT NULL,
            source_message_id TEXT NOT NULL,
            target_platform TEXT NOT NULL,
            target_message_id TEXT NOT NULL,
            target_group_id TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            UNIQUE(source_platform, media_group_id, source_message_id,
                   target_platform)
          );
        )");

                                    connection.execute(R"(
          CREATE INDEX IF NOT EXISTS idx_bridge_media_group_lookup
	          ON bridge_media_group_mappings(source_platform, media_group_id,
	                                         target_platform);
	        )");

                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            platform TEXT NOT NULL,
            user_id TEXT NOT NULL,
            group_id TEXT NOT NULL DEFAULT '',
            username TEXT NOT NULL DEFAULT '',
            nickname TEXT NOT NULL DEFAULT '',
            title TEXT NOT NULL DEFAULT '',
            first_name TEXT NOT NULL DEFAULT '',
            last_name TEXT NOT NULL DEFAULT '',
            last_updated INTEGER NOT NULL,
            UNIQUE(platform, user_id, group_id)
          );
        )");

                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_sticker_cache (
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
            conversion_status TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            last_used_at INTEGER NOT NULL,
            UNIQUE(platform, sticker_hash)
          );
        )");

                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_qq_sticker_mappings (
            qq_sticker_hash TEXT PRIMARY KEY NOT NULL,
            telegram_file_id TEXT NOT NULL,
            file_type TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            last_used_at INTEGER NOT NULL,
            is_gif INTEGER,
            content_type TEXT,
            last_checked_at INTEGER
          );
        )");

                                    connection.execute(R"(
          CREATE TABLE IF NOT EXISTS bridge_platform_heartbeats (
            platform TEXT PRIMARY KEY NOT NULL,
            last_heartbeat_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
          );
        )");
                                  });
}

auto BridgeStateRepository::add_message_mapping(
    const storage::MessageMapping &mapping) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&mapping](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          INSERT OR REPLACE INTO bridge_message_mappings
            (source_platform, source_message_id, target_platform,
             target_message_id, created_at)
          VALUES (?, ?, ?, ?, ?);
        )",
                           {mapping.source_platform, mapping.source_message_id,
                            mapping.target_platform, mapping.target_message_id,
                            timestamp_ms(mapping.created_at)});
        return true;
      });
}

auto BridgeStateRepository::get_target_message_id(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> std::optional<std::string> {
  if (source_message_id.empty()) {
    return std::nullopt;
  }

  return db_manager_.run_read<std::optional<std::string>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT target_message_id FROM bridge_message_mappings
          WHERE source_platform = ? AND source_message_id = ?
            AND target_platform = ?
          LIMIT 1;
        )",
            {source_platform, source_message_id, target_platform});
        if (rows.empty()) {
          return std::optional<std::string>{};
        }
        return std::optional<std::string>{
            std::get<std::string>(rows.front().at("target_message_id"))};
      });
}

auto BridgeStateRepository::add_message_retry(
    const storage::MessageRetryInfo &retry_info) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&retry_info](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT OR REPLACE INTO bridge_message_retry_queue
            (source_platform, target_platform, source_message_id,
             message_content, group_id, source_group_id, target_topic_id,
             retry_count, max_retry_count, failure_reason, retry_type,
             next_retry_at, created_at, last_attempt_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )",
            {retry_info.source_platform, retry_info.target_platform,
             retry_info.source_message_id, retry_info.message_content,
             retry_info.group_id, retry_info.source_group_id,
             retry_info.target_topic_id,
             static_cast<std::int64_t>(retry_info.retry_count),
             static_cast<std::int64_t>(retry_info.max_retry_count),
             retry_info.failure_reason, retry_info.retry_type,
             timestamp_ms(retry_info.next_retry_at),
             timestamp_ms(retry_info.created_at),
             timestamp_ms(retry_info.last_attempt_at)});
        return true;
      });
}

auto BridgeStateRepository::get_pending_message_retries(
    const std::chrono::system_clock::time_point &ready_at, const int limit)
    -> std::vector<storage::MessageRetryInfo> {
  return db_manager_.run_read<std::vector<storage::MessageRetryInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT source_platform, target_platform, source_message_id,
                 message_content, group_id, source_group_id, target_topic_id,
                 retry_count, max_retry_count, failure_reason, retry_type,
                 next_retry_at, created_at, last_attempt_at
          FROM bridge_message_retry_queue
          WHERE next_retry_at <= ? AND retry_count < max_retry_count
          ORDER BY next_retry_at ASC
          LIMIT ?;
        )",
            {timestamp_ms(ready_at), static_cast<std::int64_t>(limit)});

        std::vector<storage::MessageRetryInfo> retries;
        retries.reserve(rows.size());
        for (const auto &row : rows) {
          storage::MessageRetryInfo retry;
          retry.source_platform = db_string(row, "source_platform");
          retry.target_platform = db_string(row, "target_platform");
          retry.source_message_id = db_string(row, "source_message_id");
          retry.message_content = db_string(row, "message_content");
          retry.group_id = db_string(row, "group_id");
          retry.source_group_id = db_string(row, "source_group_id");
          retry.target_topic_id = db_int64(row, "target_topic_id");
          retry.retry_count = static_cast<int>(db_int64(row, "retry_count"));
          retry.max_retry_count =
              static_cast<int>(db_int64(row, "max_retry_count"));
          retry.failure_reason = db_string(row, "failure_reason");
          retry.retry_type = db_string(row, "retry_type");
          retry.next_retry_at =
              time_point_from_ms(db_int64(row, "next_retry_at"));
          retry.created_at = time_point_from_ms(db_int64(row, "created_at"));
          retry.last_attempt_at =
              time_point_from_ms(db_int64(row, "last_attempt_at"));
          retries.push_back(std::move(retry));
        }
        return retries;
      });
}

auto BridgeStateRepository::update_message_retry(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform, const int retry_count,
    const std::chrono::system_clock::time_point &next_retry_at,
    const std::string &failure_reason) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_message_retry_queue
          SET retry_count = ?, next_retry_at = ?, failure_reason = ?,
              last_attempt_at = ?
          WHERE source_platform = ? AND source_message_id = ?
            AND target_platform = ?;
        )",
                           {static_cast<std::int64_t>(retry_count),
                            timestamp_ms(next_retry_at), failure_reason,
                            timestamp_ms(std::chrono::system_clock::now()),
                            source_platform, source_message_id,
                            target_platform});
        return true;
      });
}

auto BridgeStateRepository::remove_message_retry(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          DELETE FROM bridge_message_retry_queue
          WHERE source_platform = ? AND source_message_id = ?
            AND target_platform = ?;
        )",
            {source_platform, source_message_id, target_platform});
        return true;
      });
}

auto BridgeStateRepository::add_media_group_mapping(
    const MediaGroupMapping &mapping) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&mapping](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          INSERT OR REPLACE INTO bridge_media_group_mappings
            (source_platform, media_group_id, source_message_id,
             target_platform, target_message_id, target_group_id, created_at)
          VALUES (?, ?, ?, ?, ?, ?, ?);
        )",
                           {mapping.source_platform, mapping.media_group_id,
                            mapping.source_message_id, mapping.target_platform,
                            mapping.target_message_id, mapping.target_group_id,
                            timestamp_ms(mapping.created_at)});
        return true;
      });
}

auto BridgeStateRepository::get_media_group_mappings(
    const std::string &source_platform, const std::string &media_group_id,
    const std::string &target_platform) -> std::vector<MediaGroupMapping> {
  return db_manager_.run_read<std::vector<MediaGroupMapping>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT source_platform, media_group_id, source_message_id,
                 target_platform, target_message_id, target_group_id,
                 created_at
          FROM bridge_media_group_mappings
          WHERE source_platform = ? AND media_group_id = ?
            AND target_platform = ?
          ORDER BY source_message_id ASC;
        )",
            {source_platform, media_group_id, target_platform});

        std::vector<MediaGroupMapping> mappings;
        mappings.reserve(rows.size());
        for (const auto &row : rows) {
          mappings.push_back(MediaGroupMapping{
              .source_platform = db_string(row, "source_platform"),
              .media_group_id = db_string(row, "media_group_id"),
              .source_message_id = db_string(row, "source_message_id"),
              .target_platform = db_string(row, "target_platform"),
              .target_message_id = db_string(row, "target_message_id"),
              .target_group_id = db_string(row, "target_group_id"),
              .created_at = time_point_from_ms(db_int64(row, "created_at")),
          });
        }
        return mappings;
      });
}

auto BridgeStateRepository::get_source_message_id(
    const std::string &target_platform, const std::string &target_message_id,
    const std::string &source_platform) -> std::optional<std::string> {
  if (target_message_id.empty()) {
    return std::nullopt;
  }

  return db_manager_.run_read<std::optional<std::string>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT source_message_id FROM bridge_message_mappings
          WHERE target_platform = ? AND target_message_id = ?
            AND source_platform = ?
          LIMIT 1;
        )",
            {target_platform, target_message_id, source_platform});
        if (rows.empty()) {
          return std::optional<std::string>{};
        }
        return std::optional<std::string>{
            std::get<std::string>(rows.front().at("source_message_id"))};
      });
}

auto BridgeStateRepository::update_message_mapping(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform,
    const std::string &new_target_message_id) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_message_mappings
          SET target_message_id = ?, created_at = ?
          WHERE source_platform = ? AND source_message_id = ?
            AND target_platform = ?;
        )",
                           {new_target_message_id,
                            timestamp_ms(std::chrono::system_clock::now()),
                            source_platform, source_message_id,
                            target_platform});
        return true;
      });
}

auto BridgeStateRepository::delete_message_mapping(
    const std::string &source_platform, const std::string &source_message_id,
    const std::string &target_platform) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          DELETE FROM bridge_message_mappings
          WHERE source_platform = ? AND source_message_id = ?
            AND target_platform = ?;
        )",
            {source_platform, source_message_id, target_platform});
        return true;
      });
}

auto BridgeStateRepository::save_or_update_user(
    const storage::UserInfo &user_info, const bool force_update) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto update_guard = force_update ? "" : R"(
          WHERE
            IFNULL(bridge_users.username, '') != IFNULL(excluded.username, '') OR
            IFNULL(bridge_users.nickname, '') != IFNULL(excluded.nickname, '') OR
            IFNULL(bridge_users.title, '') != IFNULL(excluded.title, '') OR
            IFNULL(bridge_users.first_name, '') != IFNULL(excluded.first_name, '') OR
            IFNULL(bridge_users.last_name, '') != IFNULL(excluded.last_name, '')
        )";
        connection.execute(std::string{R"(
          INSERT INTO bridge_users
            (platform, user_id, group_id, username, nickname, title, first_name,
             last_name, last_updated)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(platform, user_id, group_id) DO UPDATE SET
            username = excluded.username,
            nickname = excluded.nickname,
            title = excluded.title,
            first_name = excluded.first_name,
            last_name = excluded.last_name,
            last_updated = excluded.last_updated
        )"} + update_guard + ";",
                           {user_info.platform, user_info.user_id,
                            user_info.group_id, user_info.username,
                            user_info.nickname, user_info.title,
                            user_info.first_name, user_info.last_name,
                            timestamp_ms(user_info.last_updated)});
        return true;
      });
}

auto BridgeStateRepository::get_user(const std::string &platform,
                                     const std::string &user_id,
                                     const std::string &group_id)
    -> std::optional<storage::UserInfo> {
  return db_manager_.run_read<std::optional<storage::UserInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT platform, user_id, group_id, username, nickname, title,
                 first_name, last_name, last_updated
          FROM bridge_users
          WHERE platform = ? AND user_id = ? AND group_id = ?
          LIMIT 1;
        )",
            {platform, user_id, group_id});
        if (rows.empty()) {
          return std::optional<storage::UserInfo>{};
        }

        const auto &row = rows.front();
        storage::UserInfo user;
        user.platform = db_string(row, "platform");
        user.user_id = db_string(row, "user_id");
        user.group_id = db_string(row, "group_id");
        user.username = db_string(row, "username");
        user.nickname = db_string(row, "nickname");
        user.title = db_string(row, "title");
        user.first_name = db_string(row, "first_name");
        user.last_name = db_string(row, "last_name");
        user.last_updated = time_point_from_ms(db_int64(row, "last_updated"));
        return std::optional<storage::UserInfo>{std::move(user)};
      });
}

auto BridgeStateRepository::query_user_display_name(const std::string &platform,
                                                    const std::string &user_id,
                                                    const std::string &group_id)
    -> std::optional<std::string> {
  constexpr auto kUserInfoExpiration = std::chrono::minutes{10};

  const auto query_group_id = platform == "telegram" ? std::string{} : group_id;
  const auto user_info = get_user(platform, user_id, query_group_id);
  if (!user_info.has_value()) {
    return std::nullopt;
  }

  const auto &user = *user_info;
  if (std::chrono::system_clock::now() - user.last_updated >
      kUserInfoExpiration) {
    return std::nullopt;
  }

  if (platform == "telegram") {
    if (!user.nickname.empty()) {
      return user.nickname;
    }
    if (!user.first_name.empty()) {
      auto display_name = user.first_name;
      if (!user.last_name.empty()) {
        display_name += " " + user.last_name;
      }
      return display_name;
    }
    if (!user.username.empty()) {
      return user.username;
    }
    return std::nullopt;
  }

  if (!user.nickname.empty()) {
    return user.nickname;
  }
  if (!user.username.empty()) {
    return user.username;
  }
  if (!user.first_name.empty()) {
    auto display_name = user.first_name;
    if (!user.last_name.empty()) {
      display_name += " " + user.last_name;
    }
    return display_name;
  }
  return std::nullopt;
}

auto BridgeStateRepository::save_sticker_cache(
    const storage::StickerCacheInfo &cache_info) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_sticker_cache
            (platform, sticker_id, sticker_hash, original_name, file_type,
             mime_type, original_file_path, converted_file_path, container_path,
             file_size, conversion_status, created_at, last_used_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(platform, sticker_hash) DO UPDATE SET
            sticker_id = excluded.sticker_id,
            original_name = excluded.original_name,
            file_type = excluded.file_type,
            mime_type = excluded.mime_type,
            original_file_path = excluded.original_file_path,
            converted_file_path = excluded.converted_file_path,
            container_path = excluded.container_path,
            file_size = excluded.file_size,
            conversion_status = excluded.conversion_status,
            created_at = excluded.created_at,
            last_used_at = excluded.last_used_at;
        )",
            {cache_info.platform, cache_info.sticker_id,
             cache_info.sticker_hash, optional_string(cache_info.original_name),
             cache_info.file_type, optional_string(cache_info.mime_type),
             cache_info.original_file_path,
             optional_string(cache_info.converted_file_path),
             cache_info.container_path, optional_int64(cache_info.file_size),
             cache_info.conversion_status, timestamp_ms(cache_info.created_at),
             timestamp_ms(cache_info.last_used_at)});
        return true;
      });
}

auto BridgeStateRepository::get_sticker_cache(const std::string &platform,
                                              const std::string &sticker_hash)
    -> std::optional<storage::StickerCacheInfo> {
  return db_manager_.run_read<std::optional<storage::StickerCacheInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT platform, sticker_id, sticker_hash, original_name, file_type,
                 mime_type, original_file_path, converted_file_path,
                 container_path, file_size, conversion_status, created_at,
                 last_used_at
          FROM bridge_sticker_cache
          WHERE platform = ? AND sticker_hash = ?
          LIMIT 1;
        )",
            {platform, sticker_hash});
        if (rows.empty()) {
          return std::optional<storage::StickerCacheInfo>{};
        }

        const auto &row = rows.front();
        storage::StickerCacheInfo cache_info;
        cache_info.platform = db_string(row, "platform");
        cache_info.sticker_id = db_string(row, "sticker_id");
        cache_info.sticker_hash = db_string(row, "sticker_hash");
        cache_info.original_name = db_optional_string(row, "original_name");
        cache_info.file_type = db_string(row, "file_type");
        cache_info.mime_type = db_optional_string(row, "mime_type");
        cache_info.original_file_path = db_string(row, "original_file_path");
        cache_info.converted_file_path =
            db_optional_string(row, "converted_file_path");
        cache_info.container_path = db_string(row, "container_path");
        cache_info.file_size = db_optional_int64(row, "file_size");
        cache_info.conversion_status = db_string(row, "conversion_status");
        cache_info.created_at = time_point_from_ms(db_int64(row, "created_at"));
        cache_info.last_used_at =
            time_point_from_ms(db_int64(row, "last_used_at"));
        return std::optional<storage::StickerCacheInfo>{std::move(cache_info)};
      });
}

auto BridgeStateRepository::update_sticker_last_used(
    const std::string &platform, const std::string &sticker_hash) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          UPDATE bridge_sticker_cache
          SET last_used_at = ?
          WHERE platform = ? AND sticker_hash = ?;
        )",
            {timestamp_ms(std::chrono::system_clock::now()), platform,
             sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::update_sticker_conversion(
    const std::string &platform, const std::string &sticker_hash,
    const std::string &conversion_status,
    const std::optional<std::string> &converted_file_path) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          UPDATE bridge_sticker_cache
          SET conversion_status = ?, converted_file_path = ?
          WHERE platform = ? AND sticker_hash = ?;
        )",
            {conversion_status, optional_string(converted_file_path), platform,
             sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::save_qq_sticker_mapping(
    const storage::QQStickerMapping &mapping) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_qq_sticker_mappings
            (qq_sticker_hash, telegram_file_id, file_type, created_at,
             last_used_at, is_gif, content_type, last_checked_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(qq_sticker_hash) DO UPDATE SET
            telegram_file_id = excluded.telegram_file_id,
            file_type = excluded.file_type,
            created_at = excluded.created_at,
            last_used_at = excluded.last_used_at,
            is_gif = excluded.is_gif,
            content_type = excluded.content_type,
            last_checked_at = excluded.last_checked_at;
        )",
            {mapping.qq_sticker_hash, mapping.telegram_file_id,
             mapping.file_type, timestamp_ms(mapping.created_at),
             timestamp_ms(mapping.last_used_at), optional_bool(mapping.is_gif),
             optional_string(mapping.content_type),
             optional_time_ms(mapping.last_checked_at)});
        return true;
      });
}

auto BridgeStateRepository::get_qq_sticker_mapping(
    const std::string &qq_sticker_hash)
    -> std::optional<storage::QQStickerMapping> {
  return db_manager_.run_read<std::optional<storage::QQStickerMapping>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT qq_sticker_hash, telegram_file_id, file_type, created_at,
                 last_used_at, is_gif, content_type, last_checked_at
          FROM bridge_qq_sticker_mappings
          WHERE qq_sticker_hash = ?
          LIMIT 1;
        )",
            {qq_sticker_hash});
        if (rows.empty()) {
          return std::optional<storage::QQStickerMapping>{};
        }

        const auto &row = rows.front();
        storage::QQStickerMapping mapping;
        mapping.qq_sticker_hash = db_string(row, "qq_sticker_hash");
        mapping.telegram_file_id = db_string(row, "telegram_file_id");
        mapping.file_type = db_string(row, "file_type");
        mapping.created_at = time_point_from_ms(db_int64(row, "created_at"));
        mapping.last_used_at =
            time_point_from_ms(db_int64(row, "last_used_at"));
        mapping.is_gif = db_optional_bool(row, "is_gif");
        mapping.content_type = db_optional_string(row, "content_type");
        mapping.last_checked_at = db_optional_time(row, "last_checked_at");
        return std::optional<storage::QQStickerMapping>{std::move(mapping)};
      });
}

auto BridgeStateRepository::update_qq_sticker_last_used(
    const std::string &qq_sticker_hash) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          UPDATE bridge_qq_sticker_mappings
          SET last_used_at = ?
          WHERE qq_sticker_hash = ?;
        )",
            {timestamp_ms(std::chrono::system_clock::now()), qq_sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::update_platform_heartbeat(
    const std::string &platform,
    const std::chrono::system_clock::time_point &heartbeat_time) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_platform_heartbeats
            (platform, last_heartbeat_at, updated_at)
          VALUES (?, ?, ?)
          ON CONFLICT(platform) DO UPDATE SET
            last_heartbeat_at = excluded.last_heartbeat_at,
            updated_at = excluded.updated_at;
        )",
            {platform, timestamp_ms(heartbeat_time),
             timestamp_ms(std::chrono::system_clock::now())});
        return true;
      });
}

auto BridgeStateRepository::get_platform_heartbeat(const std::string &platform)
    -> std::optional<storage::PlatformHeartbeatInfo> {
  return db_manager_.run_read<std::optional<storage::PlatformHeartbeatInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT platform, last_heartbeat_at, updated_at
          FROM bridge_platform_heartbeats
          WHERE platform = ?
          LIMIT 1;
        )",
            {platform});
        if (rows.empty()) {
          return std::optional<storage::PlatformHeartbeatInfo>{};
        }

        const auto &row = rows.front();
        return std::optional<storage::PlatformHeartbeatInfo>{
            storage::PlatformHeartbeatInfo{
                .platform = db_string(row, "platform"),
                .last_heartbeat_at =
                    time_point_from_ms(db_int64(row, "last_heartbeat_at")),
                .updated_at = time_point_from_ms(db_int64(row, "updated_at")),
            }};
      });
}

} // namespace bridge

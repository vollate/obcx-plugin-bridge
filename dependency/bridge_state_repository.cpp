#include "bridge_state_repository.hpp"

#include <common/logger.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace bridge {
namespace {

constexpr std::string_view kSchemaTable = "bridge_schema_version";
constexpr std::string_view kMappingsTable = "bridge_message_mappings";
constexpr std::string_view kRetriesTable = "bridge_message_retry_queue";
constexpr std::string_view kMediaGroupsTable = "bridge_media_group_mappings";
constexpr std::string_view kUsersTable = "bridge_users";
constexpr std::string_view kStickerCacheTable = "bridge_sticker_cache";
constexpr std::string_view kQqStickerTable = "bridge_qq_sticker_mappings";
constexpr std::string_view kHeartbeatsTable = "bridge_platform_heartbeats";
constexpr std::string_view kMappingsArchiveTable =
    "bridge_message_mappings_v2_archive";
constexpr std::string_view kMediaGroupsArchiveTable =
    "bridge_media_group_mappings_v2_archive";

const std::vector<std::string> kStateTables = {
    std::string{kMappingsTable},     std::string{kRetriesTable},
    std::string{kMediaGroupsTable},  std::string{kUsersTable},
    std::string{kStickerCacheTable}, std::string{kQqStickerTable},
    std::string{kHeartbeatsTable},
};

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

auto table_exists(obcx::core::IDbConnection &connection,
                  const std::string_view table) -> bool {
  return !connection
              .query("SELECT name FROM sqlite_master WHERE type = 'table' "
                     "AND name = ?;",
                     {std::string{table}})
              .empty();
}

auto index_exists(obcx::core::IDbConnection &connection,
                  const std::string_view index) -> bool {
  return !connection
              .query("SELECT name FROM sqlite_master WHERE type = 'index' "
                     "AND name = ?;",
                     {std::string{index}})
              .empty();
}

auto table_row_count(obcx::core::IDbConnection &connection,
                     const std::string &table) -> std::int64_t {
  if (!table_exists(connection, table)) {
    return 0;
  }
  const auto rows =
      connection.query("SELECT COUNT(*) AS count FROM \"" + table + "\";");
  return rows.empty() ? 0 : db_int64(rows.front(), "count");
}

void require_columns(obcx::core::IDbConnection &connection,
                     const std::string &table,
                     const std::vector<std::string> &required) {
  if (!table_exists(connection, table)) {
    throw std::runtime_error("bridge schema is missing table " + table);
  }
  const auto rows = connection.query("PRAGMA table_info(\"" + table + "\");");
  std::unordered_set<std::string> columns;
  for (const auto &row : rows) {
    columns.insert(db_string(row, "name"));
  }
  for (const auto &column : required) {
    if (!columns.contains(column)) {
      throw std::runtime_error("bridge schema table " + table +
                               " is missing column " + column);
    }
  }
}

void create_v2_tables(obcx::core::IDbConnection &connection) {
  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_message_mappings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, source_message_id,
             target_installation, target_platform)
    );
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_message_retry_queue (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      target_installation TEXT NOT NULL,
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
      UNIQUE(source_installation, source_platform, source_message_id,
             target_installation, target_platform)
    );
  )");
  connection.execute(R"(
    CREATE INDEX IF NOT EXISTS idx_bridge_message_retry_next_retry
    ON bridge_message_retry_queue(next_retry_at);
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_media_group_mappings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      media_group_id TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      target_group_id TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, media_group_id,
             source_message_id, target_installation, target_platform)
    );
  )");
  connection.execute(R"(
    CREATE INDEX IF NOT EXISTS idx_bridge_media_group_lookup
    ON bridge_media_group_mappings(source_installation, source_platform,
                                   media_group_id, target_installation,
                                   target_platform);
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_users (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      installation_id TEXT NOT NULL,
      platform TEXT NOT NULL,
      user_id TEXT NOT NULL,
      group_id TEXT NOT NULL DEFAULT '',
      username TEXT NOT NULL DEFAULT '',
      nickname TEXT NOT NULL DEFAULT '',
      title TEXT NOT NULL DEFAULT '',
      first_name TEXT NOT NULL DEFAULT '',
      last_name TEXT NOT NULL DEFAULT '',
      last_updated INTEGER NOT NULL,
      UNIQUE(installation_id, platform, user_id, group_id)
    );
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_sticker_cache (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      installation_id TEXT NOT NULL,
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
      UNIQUE(installation_id, platform, sticker_hash)
    );
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_qq_sticker_mappings (
      source_installation TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      qq_sticker_hash TEXT NOT NULL,
      telegram_file_id TEXT NOT NULL,
      file_type TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      last_used_at INTEGER NOT NULL,
      is_gif INTEGER,
      content_type TEXT,
      last_checked_at INTEGER,
      PRIMARY KEY(source_installation, target_installation, qq_sticker_hash)
    );
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_platform_heartbeats (
      installation_id TEXT PRIMARY KEY NOT NULL,
      platform TEXT NOT NULL,
      last_heartbeat_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL
    );
  )");
}

void validate_v2_shape(obcx::core::IDbConnection &connection) {
  require_columns(connection, std::string{kMappingsTable},
                  {"source_installation", "source_platform",
                   "source_message_id", "target_installation",
                   "target_platform", "target_message_id"});
  require_columns(connection, std::string{kRetriesTable},
                  {"source_installation", "source_platform",
                   "target_installation", "target_platform",
                   "source_message_id"});
  require_columns(connection, std::string{kMediaGroupsTable},
                  {"source_installation", "source_platform", "media_group_id",
                   "target_installation", "target_platform"});
  require_columns(connection, std::string{kUsersTable},
                  {"installation_id", "platform", "user_id"});
  require_columns(connection, std::string{kStickerCacheTable},
                  {"installation_id", "platform", "sticker_hash"});
  require_columns(
      connection, std::string{kQqStickerTable},
      {"source_installation", "target_installation", "qq_sticker_hash"});
  require_columns(connection, std::string{kHeartbeatsTable},
                  {"installation_id", "platform", "last_heartbeat_at"});
}

void reject_unknown_platforms(obcx::core::IDbConnection &connection,
                              const std::string &table,
                              const std::vector<std::string> &columns) {
  if (!table_exists(connection, table)) {
    return;
  }
  for (const auto &column : columns) {
    const auto rows = connection.query("SELECT COUNT(*) AS count FROM \"" +
                                       table + "\" WHERE \"" + column +
                                       "\" NOT IN ('qq', 'telegram');");
    if (!rows.empty() && db_int64(rows.front(), "count") != 0) {
      throw std::runtime_error("bridge legacy table " + table +
                               " contains unsupported platform values");
    }
  }
}

void require_installation(const std::string &installation,
                          const std::string_view field) {
  if (installation.empty()) {
    throw std::invalid_argument("bridge state requires " + std::string{field});
  }
}

auto backup_name(const std::string &table) -> std::string {
  return table + "_obcx_v1";
}

void rename_legacy_tables(obcx::core::IDbConnection &connection) {
  for (const auto &table : kStateTables) {
    if (!table_exists(connection, table)) {
      continue;
    }
    const auto backup = backup_name(table);
    if (table_exists(connection, backup)) {
      throw std::runtime_error(
          "bridge legacy migration backup already exists: " + backup);
    }
    connection.execute("ALTER TABLE \"" + table + "\" RENAME TO \"" + backup +
                       "\";");
  }
  // Named indexes follow renamed SQLite tables and would prevent the version-2
  // definitions from being created under the same names.
  connection.execute(
      "DROP INDEX IF EXISTS idx_bridge_message_retry_next_retry;");
  connection.execute("DROP INDEX IF EXISTS idx_bridge_media_group_lookup;");
}

void verify_and_drop_backup(obcx::core::IDbConnection &connection,
                            const std::string &table,
                            const std::int64_t expected) {
  if (table_row_count(connection, table) != expected) {
    throw std::runtime_error("bridge migration row count mismatch for " +
                             table);
  }
  const auto backup = backup_name(table);
  if (table_exists(connection, backup)) {
    connection.execute("DROP TABLE \"" + backup + "\";");
  }
}

void migrate_v1_tables(
    obcx::core::IDbConnection &connection,
    const BridgeStateMigrationContext &migration,
    const std::unordered_map<std::string, std::int64_t> &legacy_counts) {
  reject_unknown_platforms(connection, std::string{kMappingsTable},
                           {"source_platform", "target_platform"});
  reject_unknown_platforms(connection, std::string{kRetriesTable},
                           {"source_platform", "target_platform"});
  reject_unknown_platforms(connection, std::string{kMediaGroupsTable},
                           {"source_platform", "target_platform"});
  reject_unknown_platforms(connection, std::string{kUsersTable}, {"platform"});
  reject_unknown_platforms(connection, std::string{kStickerCacheTable},
                           {"platform"});
  reject_unknown_platforms(connection, std::string{kHeartbeatsTable},
                           {"platform"});

  rename_legacy_tables(connection);
  create_v2_tables(connection);

  if (table_exists(connection, backup_name(std::string{kMappingsTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_message_mappings
        (id, source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      SELECT id,
             CASE source_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             source_platform, source_message_id,
             CASE target_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             target_platform, target_message_id, created_at
      FROM bridge_message_mappings_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation,
         migration.telegram_installation, migration.onebot11_installation});
  }

  if (table_exists(connection, backup_name(std::string{kRetriesTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_message_retry_queue
        (id, source_installation, source_platform, target_installation,
         target_platform, source_message_id, message_content, group_id,
         source_group_id, target_topic_id, retry_count, max_retry_count,
         failure_reason, retry_type, next_retry_at, created_at, last_attempt_at)
      SELECT id,
             CASE source_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             source_platform,
             CASE target_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             target_platform, source_message_id, message_content, group_id,
             source_group_id, target_topic_id, retry_count, max_retry_count,
             failure_reason, retry_type, next_retry_at, created_at,
             last_attempt_at
      FROM bridge_message_retry_queue_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation,
         migration.telegram_installation, migration.onebot11_installation});
  }

  if (table_exists(connection, backup_name(std::string{kMediaGroupsTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_media_group_mappings
        (id, source_installation, source_platform, media_group_id,
         source_message_id, target_installation, target_platform,
         target_message_id, target_group_id, created_at)
      SELECT id,
             CASE source_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             source_platform, media_group_id, source_message_id,
             CASE target_platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             target_platform, target_message_id, target_group_id, created_at
      FROM bridge_media_group_mappings_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation,
         migration.telegram_installation, migration.onebot11_installation});
  }

  if (table_exists(connection, backup_name(std::string{kUsersTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_users
        (id, installation_id, platform, user_id, group_id, username, nickname,
         title, first_name, last_name, last_updated)
      SELECT id, CASE platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             platform, user_id, group_id, username, nickname, title,
             first_name, last_name, last_updated
      FROM bridge_users_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation});
  }

  if (table_exists(connection, backup_name(std::string{kStickerCacheTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_sticker_cache
        (id, installation_id, platform, sticker_id, sticker_hash,
         original_name, file_type, mime_type, original_file_path,
         converted_file_path, container_path, file_size, conversion_status,
         created_at, last_used_at)
      SELECT id, CASE platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             platform, sticker_id, sticker_hash, original_name, file_type,
             mime_type, original_file_path, converted_file_path, container_path,
             file_size, conversion_status, created_at, last_used_at
      FROM bridge_sticker_cache_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation});
  }

  if (table_exists(connection, backup_name(std::string{kQqStickerTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_qq_sticker_mappings
        (source_installation, target_installation, qq_sticker_hash,
         telegram_file_id, file_type, created_at, last_used_at, is_gif,
         content_type, last_checked_at)
      SELECT ?, ?, qq_sticker_hash, telegram_file_id, file_type, created_at,
             last_used_at, is_gif, content_type, last_checked_at
      FROM bridge_qq_sticker_mappings_obcx_v1;
    )",
        {migration.onebot11_installation, migration.telegram_installation});
  }

  if (table_exists(connection, backup_name(std::string{kHeartbeatsTable}))) {
    connection.execute(
        R"(
      INSERT INTO bridge_platform_heartbeats
        (installation_id, platform, last_heartbeat_at, updated_at)
      SELECT CASE platform WHEN 'telegram' THEN ? WHEN 'qq' THEN ? END,
             platform, last_heartbeat_at, updated_at
      FROM bridge_platform_heartbeats_obcx_v1;
    )",
        {migration.telegram_installation, migration.onebot11_installation});
  }

  for (const auto &table : kStateTables) {
    verify_and_drop_backup(connection, table, legacy_counts.at(table));
  }
}

void create_v3_message_tables(obcx::core::IDbConnection &connection) {
  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_message_mappings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_conversation_id TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_conversation_id TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      is_primary INTEGER NOT NULL DEFAULT 1 CHECK(is_primary IN (0, 1)),
      created_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, source_conversation_id,
             source_message_id, target_installation, target_platform,
             target_conversation_id)
    );
  )");
  connection.execute(R"(
    CREATE INDEX IF NOT EXISTS idx_bridge_message_mapping_reverse
    ON bridge_message_mappings(
      target_installation, target_platform, target_conversation_id,
      target_message_id, source_installation, source_platform,
      source_conversation_id, is_primary);
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_message_retry_queue (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_conversation_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_conversation_id TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      message_content TEXT NOT NULL,
      group_id TEXT NOT NULL,
      source_group_id TEXT NOT NULL,
      target_topic_id INTEGER DEFAULT -1,
      retry_count INTEGER NOT NULL DEFAULT 0,
      max_retry_count INTEGER NOT NULL DEFAULT 5,
      failure_reason TEXT,
      retry_type TEXT NOT NULL DEFAULT 'message_send',
      next_retry_at INTEGER NOT NULL,
      created_at INTEGER NOT NULL,
      last_attempt_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, source_conversation_id,
             source_message_id, target_installation, target_platform,
             target_conversation_id)
    );
  )");
  connection.execute(R"(
    CREATE INDEX IF NOT EXISTS idx_bridge_message_retry_next_retry
    ON bridge_message_retry_queue(next_retry_at);
  )");

  connection.execute(R"(
    CREATE TABLE IF NOT EXISTS bridge_media_group_mappings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_conversation_id TEXT NOT NULL,
      media_group_id TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_conversation_id TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      target_group_id TEXT NOT NULL,
      is_primary INTEGER NOT NULL DEFAULT 0 CHECK(is_primary IN (0, 1)),
      created_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, source_conversation_id,
             media_group_id, source_message_id, target_installation,
             target_platform, target_conversation_id)
    );
  )");
  connection.execute(R"(
    CREATE INDEX IF NOT EXISTS idx_bridge_media_group_lookup
    ON bridge_media_group_mappings(
      source_installation, source_platform, source_conversation_id,
      media_group_id, target_installation, target_platform,
      target_conversation_id);
  )");
}

void validate_v3_shape(obcx::core::IDbConnection &connection) {
  validate_v2_shape(connection);
  require_columns(
      connection, std::string{kMappingsTable},
      {"source_conversation_id", "target_conversation_id", "is_primary"});
  require_columns(
      connection, std::string{kRetriesTable},
      {"source_conversation_id", "target_conversation_id", "source_group_id"});
  require_columns(
      connection, std::string{kMediaGroupsTable},
      {"source_conversation_id", "target_conversation_id", "is_primary"});
  for (const auto index : {"idx_bridge_message_mapping_reverse",
                           "idx_bridge_message_retry_next_retry",
                           "idx_bridge_media_group_lookup"}) {
    if (!index_exists(connection, index)) {
      throw std::runtime_error("bridge schema is missing index " +
                               std::string{index});
    }
  }
  const auto invalid_mapping_primary = connection.query(R"(
    SELECT COUNT(*) AS count FROM (
      SELECT target_installation, target_platform, target_conversation_id,
             target_message_id, source_installation, source_platform,
             source_conversation_id
      FROM bridge_message_mappings
      GROUP BY target_installation, target_platform, target_conversation_id,
               target_message_id, source_installation, source_platform,
               source_conversation_id
      HAVING SUM(is_primary) != 1
    );
  )");
  if (!invalid_mapping_primary.empty() &&
      db_int64(invalid_mapping_primary.front(), "count") != 0) {
    throw std::runtime_error(
        "bridge schema contains invalid message mapping primary ownership");
  }
  const auto invalid_media_primary = connection.query(R"(
    SELECT COUNT(*) AS count FROM (
      SELECT source_installation, source_platform, source_conversation_id,
             media_group_id, target_installation, target_platform,
             target_conversation_id, target_message_id
      FROM bridge_media_group_mappings
      GROUP BY source_installation, source_platform, source_conversation_id,
               media_group_id, target_installation, target_platform,
               target_conversation_id, target_message_id
      HAVING SUM(is_primary) != 1
    );
  )");
  if (!invalid_media_primary.empty() &&
      db_int64(invalid_media_primary.front(), "count") != 0) {
    throw std::runtime_error(
        "bridge schema contains invalid media-group primary ownership");
  }
}

auto v2_backup_name(const std::string_view table) -> std::string {
  return std::string{table} + "_obcx_v2";
}

void rename_v2_message_tables(obcx::core::IDbConnection &connection) {
  for (const auto table : {kMappingsTable, kRetriesTable, kMediaGroupsTable}) {
    const auto backup = v2_backup_name(table);
    if (!table_exists(connection, table)) {
      throw std::runtime_error("bridge schema is missing version-2 table " +
                               std::string{table});
    }
    if (table_exists(connection, backup)) {
      throw std::runtime_error("bridge version-2 migration backup exists: " +
                               backup);
    }
    connection.execute("ALTER TABLE \"" + std::string{table} +
                       "\" RENAME TO \"" + backup + "\";");
  }
  connection.execute(
      "DROP INDEX IF EXISTS idx_bridge_message_retry_next_retry;");
  connection.execute("DROP INDEX IF EXISTS idx_bridge_media_group_lookup;");
  connection.execute(
      "DROP INDEX IF EXISTS idx_bridge_message_mapping_reverse;");
}

auto safe_identifier_part(const std::string &value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte) || ch == '_') {
      result.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      continue;
    }
    throw std::invalid_argument(
        "bridge migration Message Store namespace is invalid");
  }
  if (result.empty()) {
    throw std::invalid_argument(
        "bridge migration Message Store namespace is empty");
  }
  return result;
}

struct SourceEvidence {
  std::string conversation_id;
  std::optional<std::int64_t> topic_id;
  std::string media_group_id;
  bool primary_evidence = false;
};

using SourceEvidenceMap =
    std::unordered_map<std::string, std::vector<SourceEvidence>>;

auto evidence_key(const std::string_view platform, const std::string_view bot,
                  const std::string_view message_id) -> std::string {
  return std::string{platform} + '\x1f' + std::string{bot} + '\x1f' +
         std::string{message_id};
}

auto parse_json(const std::string &text) -> nlohmann::json {
  if (text.empty()) {
    return nlohmann::json::object();
  }
  auto result = nlohmann::json::parse(text, nullptr, false);
  return result.is_discarded() ? nlohmann::json::object() : std::move(result);
}

auto nested_value(const nlohmann::json &json, const std::string_view key)
    -> const nlohmann::json * {
  if (!json.is_object()) {
    return nullptr;
  }
  const auto field = std::string{key};
  if (json.contains(field)) {
    return &json.at(field);
  }
  for (const auto nested : {"data", "message", "edited_message", "payload"}) {
    if (json.contains(nested)) {
      if (const auto *value = nested_value(json.at(nested), key)) {
        return value;
      }
    }
  }
  return nullptr;
}

auto json_int64(const nlohmann::json &json, const std::string_view key)
    -> std::optional<std::int64_t> {
  const auto *value = nested_value(json, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->is_number_integer()) {
    return value->get<std::int64_t>();
  }
  if (value->is_number_unsigned()) {
    return static_cast<std::int64_t>(value->get<std::uint64_t>());
  }
  if (value->is_string()) {
    try {
      return std::stoll(value->get<std::string>());
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

auto json_string(const nlohmann::json &json, const std::string_view key)
    -> std::string {
  const auto *value = nested_value(json, key);
  if (value == nullptr) {
    return {};
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  if (value->is_number_integer()) {
    return std::to_string(value->get<std::int64_t>());
  }
  if (value->is_number_unsigned()) {
    return std::to_string(value->get<std::uint64_t>());
  }
  return {};
}

auto has_primary_album_evidence(const nlohmann::json &json) -> bool {
  const auto caption = json_string(json, "caption");
  if (!caption.empty() || nested_value(json, "reply_to_message") != nullptr ||
      nested_value(json, "reply_to_message_id") != nullptr) {
    return true;
  }
  return false;
}

auto load_source_evidence(obcx::core::IDbConnection &connection,
                          const BridgeStateMigrationContext &migration)
    -> SourceEvidenceMap {
  connection.execute(R"(
    CREATE TEMP TABLE bridge_v3_source_keys (
      source_platform TEXT NOT NULL,
      source_bot TEXT NOT NULL,
      message_id TEXT NOT NULL,
      PRIMARY KEY(source_platform, source_bot, message_id)
    );
  )");
  for (const auto table : {kMappingsTable, kMediaGroupsTable}) {
    const auto backup = v2_backup_name(table);
    connection.execute(
        "INSERT OR IGNORE INTO bridge_v3_source_keys "
        "SELECT source_platform, source_installation, source_message_id "
        "FROM \"" +
        backup + "\";");
  }

  connection.execute(R"(
    CREATE TEMP TABLE bridge_v3_message_sources (
      source_platform TEXT NOT NULL,
      source_bot TEXT NOT NULL,
      conversation_id TEXT NOT NULL,
      message_id TEXT NOT NULL,
      payload TEXT NOT NULL,
      raw TEXT NOT NULL
    );
  )");
  const auto prefix = safe_identifier_part(migration.message_store_namespace);
  const auto qq_table = prefix + "_qq_messages";
  const auto telegram_table = prefix + "_telegram_messages";
  if (table_exists(connection, qq_table)) {
    connection.execute(
        "INSERT INTO bridge_v3_message_sources "
        "SELECT m.source_platform, m.source_bot, m.conversation_id, "
        "m.message_id, '', '' FROM \"" +
        qq_table +
        "\" m JOIN bridge_v3_source_keys k ON "
        "k.source_platform=m.source_platform AND k.source_bot=m.source_bot "
        "AND k.message_id=m.message_id;");
  }
  if (table_exists(connection, telegram_table)) {
    connection.execute(
        "INSERT INTO bridge_v3_message_sources "
        "SELECT m.source_platform, m.source_bot, m.conversation_id, "
        "m.message_id, m.payload, m.raw FROM \"" +
        telegram_table +
        "\" m JOIN bridge_v3_source_keys k ON "
        "k.source_platform=m.source_platform AND k.source_bot=m.source_bot "
        "AND k.message_id=m.message_id;");
  }
  connection.execute(R"(
    CREATE INDEX bridge_v3_message_sources_lookup
    ON bridge_v3_message_sources(source_platform, source_bot, message_id);
  )");

  SourceEvidenceMap result;
  for (const auto &row : connection.query(
           "SELECT source_platform, source_bot, conversation_id, message_id, "
           "payload, raw FROM bridge_v3_message_sources;")) {
    const auto payload = parse_json(db_string(row, "payload"));
    const auto raw = parse_json(db_string(row, "raw"));
    const auto topic =
        json_int64(payload, "message_thread_id")
            .value_or(json_int64(raw, "message_thread_id").value_or(-1));
    auto media_group = json_string(payload, "media_group_id");
    if (media_group.empty()) {
      media_group = json_string(raw, "media_group_id");
    }
    result[evidence_key(db_string(row, "source_platform"),
                        db_string(row, "source_bot"),
                        db_string(row, "message_id"))]
        .push_back({.conversation_id = db_string(row, "conversation_id"),
                    .topic_id = topic > 0 ? std::optional<std::int64_t>{topic}
                                          : std::nullopt,
                    .media_group_id = std::move(media_group),
                    .primary_evidence = has_primary_album_evidence(payload) ||
                                        has_primary_album_evidence(raw)});
  }
  return result;
}

struct ResolvedConversations {
  bool resolved = false;
  std::string source;
  std::string target;
  bool primary_evidence = false;
  std::string reason;
};

auto resolve_conversations(const BridgeStateMigrationContext &migration,
                           const SourceEvidenceMap &evidence,
                           const std::string &source_installation,
                           const std::string &source_platform,
                           const std::string &source_message_id,
                           const std::string &target_installation,
                           const std::string &target_platform)
    -> ResolvedConversations {
  if ((source_platform != "qq" && source_platform != "telegram") ||
      (target_platform != "qq" && target_platform != "telegram") ||
      source_platform == target_platform) {
    return {.reason = "unsupported_platform_direction"};
  }
  const auto found = evidence.find(
      evidence_key(source_platform, source_installation, source_message_id));
  if (found == evidence.end() || found->second.empty()) {
    return {.reason = "source_history_missing"};
  }

  struct Candidate {
    std::string source;
    std::string target;
    bool primary_evidence = false;
  };
  std::vector<Candidate> candidates;
  for (const auto &source : found->second) {
    std::vector<const LegacyConversationRoute *> routes;
    for (const auto &route : migration.conversation_routes) {
      if (source_platform == "qq") {
        if (route.onebot11_installation == source_installation &&
            route.telegram_installation == target_installation &&
            target_platform == "telegram" &&
            route.qq_conversation_id == source.conversation_id) {
          routes.push_back(&route);
        }
      } else if (route.telegram_installation == source_installation &&
                 route.onebot11_installation == target_installation &&
                 target_platform == "qq" &&
                 route.telegram_conversation_id == source.conversation_id) {
        routes.push_back(&route);
      }
    }
    if (source_platform == "telegram") {
      if (source.topic_id.has_value()) {
        const auto has_exact_topic_route =
            std::ranges::any_of(routes, [&](const auto *route) {
              return route->telegram_topic_id == *source.topic_id;
            });
        std::erase_if(routes, [&](const auto *route) {
          if (has_exact_topic_route) {
            return route->telegram_topic_id != *source.topic_id;
          }
          // A forum topic is still part of its containing chat. Group-to-group
          // routes deliberately use -1 and therefore apply regardless of the
          // Telegram thread metadata carried by an individual message.
          return route->telegram_topic_id != -1;
        });
      } else {
        // Never guess a topic-to-group route when Message Store has no exact
        // thread evidence. A chat-wide group route remains deterministic.
        std::erase_if(routes, [](const auto *route) {
          return route->telegram_topic_id != -1;
        });
      }
    }
    for (const auto *route : routes) {
      candidates.push_back({.source = source.conversation_id,
                            .target = source_platform == "qq"
                                          ? route->telegram_conversation_id
                                          : route->qq_conversation_id,
                            .primary_evidence = source.primary_evidence});
    }
  }

  std::map<std::pair<std::string, std::string>, bool> distinct;
  for (const auto &candidate : candidates) {
    distinct[{candidate.source, candidate.target}] =
        distinct[{candidate.source, candidate.target}] ||
        candidate.primary_evidence;
  }
  if (distinct.empty()) {
    return {.reason = "route_history_missing"};
  }
  if (distinct.size() != 1) {
    return {.reason = "source_conversation_ambiguous"};
  }
  return {.resolved = true,
          .source = distinct.begin()->first.first,
          .target = distinct.begin()->first.second,
          .primary_evidence = distinct.begin()->second};
}

struct PlannedMapping {
  obcx::core::DbRow row;
  ResolvedConversations conversations;
  bool primary = true;
};

struct PlannedMediaMapping {
  obcx::core::DbRow row;
  ResolvedConversations conversations;
  bool primary = false;
};

void create_v2_archives(obcx::core::IDbConnection &connection) {
  connection.execute(R"(
    CREATE TABLE bridge_message_mappings_v2_archive (
      original_id INTEGER NOT NULL,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      reason TEXT NOT NULL
    );
  )");
  connection.execute(R"(
    CREATE TABLE bridge_media_group_mappings_v2_archive (
      original_id INTEGER NOT NULL,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      media_group_id TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      target_group_id TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      reason TEXT NOT NULL
    );
  )");
}

auto migration_failure_summary(
    const std::map<std::string, std::int64_t> &reasons) -> std::string {
  std::string result = "bridge schema 2 -> 3 has unresolved state";
  for (const auto &[reason, count] : reasons) {
    result += " " + reason + "=" + std::to_string(count);
  }
  return result;
}

void migrate_v2_message_state(obcx::core::IDbConnection &connection,
                              const BridgeStateMigrationContext &migration) {
  validate_v2_shape(connection);
  const auto mapping_count =
      table_row_count(connection, std::string{kMappingsTable});
  const auto retry_count =
      table_row_count(connection, std::string{kRetriesTable});
  const auto media_count =
      table_row_count(connection, std::string{kMediaGroupsTable});

  rename_v2_message_tables(connection);
  create_v3_message_tables(connection);
  const auto evidence = load_source_evidence(connection, migration);

  std::vector<PlannedMapping> mappings;
  std::map<std::string, std::int64_t> reasons;
  for (auto row :
       connection.query("SELECT * FROM bridge_message_mappings_obcx_v2;")) {
    auto conversations = resolve_conversations(
        migration, evidence, db_string(row, "source_installation"),
        db_string(row, "source_platform"), db_string(row, "source_message_id"),
        db_string(row, "target_installation"),
        db_string(row, "target_platform"));
    if (!conversations.resolved) {
      ++reasons[conversations.reason];
    }
    mappings.push_back(
        {.row = std::move(row), .conversations = std::move(conversations)});
  }

  std::unordered_map<std::string, std::unordered_set<std::string>>
      native_target_conversations;
  for (const auto &mapping : mappings) {
    if (!mapping.conversations.resolved) {
      continue;
    }
    const auto native_key = db_string(mapping.row, "target_installation") +
                            '\x1f' + db_string(mapping.row, "target_platform") +
                            '\x1f' +
                            db_string(mapping.row, "target_message_id");
    native_target_conversations[native_key].insert(
        mapping.conversations.target);
  }
  const auto cross_conversation_collisions =
      std::ranges::count_if(native_target_conversations, [](const auto &entry) {
        return entry.second.size() > 1;
      });
  if (cross_conversation_collisions != 0) {
    OBCX_INFO("Bridge migration preserving {} cross-conversation native-id "
              "collisions",
              cross_conversation_collisions);
  }

  std::unordered_map<std::string, std::vector<std::size_t>> target_groups;
  for (std::size_t index = 0; index < mappings.size(); ++index) {
    const auto &mapping = mappings[index];
    if (!mapping.conversations.resolved) {
      continue;
    }
    const auto key = db_string(mapping.row, "target_installation") + '\x1f' +
                     db_string(mapping.row, "target_platform") + '\x1f' +
                     mapping.conversations.target + '\x1f' +
                     db_string(mapping.row, "target_message_id") + '\x1f' +
                     db_string(mapping.row, "source_installation") + '\x1f' +
                     db_string(mapping.row, "source_platform") + '\x1f' +
                     mapping.conversations.source;
    target_groups[key].push_back(index);
  }
  for (const auto &[_, indices] : target_groups) {
    if (indices.size() == 1) {
      mappings[indices.front()].primary = true;
      continue;
    }
    std::size_t primary_count = 0;
    std::size_t primary_index = 0;
    for (const auto index : indices) {
      if (mappings[index].conversations.primary_evidence) {
        ++primary_count;
        primary_index = index;
      }
    }
    if (primary_count == 1) {
      for (const auto index : indices) {
        mappings[index].primary = index == primary_index;
      }
      continue;
    }
    for (const auto index : indices) {
      mappings[index].conversations.resolved = false;
      mappings[index].conversations.reason = "fan_in_primary_unresolved";
      ++reasons["fan_in_primary_unresolved"];
    }
  }

  std::vector<PlannedMediaMapping> media;
  for (auto row :
       connection.query("SELECT * FROM bridge_media_group_mappings_obcx_v2;")) {
    auto conversations = resolve_conversations(
        migration, evidence, db_string(row, "source_installation"),
        db_string(row, "source_platform"), db_string(row, "source_message_id"),
        db_string(row, "target_installation"),
        db_string(row, "target_platform"));
    if (conversations.resolved) {
      const auto expected_target = canonical_conversation_id(
          db_string(row, "target_platform"), db_string(row, "target_group_id"));
      if (conversations.target != expected_target) {
        conversations = {.reason = "media_target_route_mismatch"};
      }
    }
    if (!conversations.resolved) {
      ++reasons[conversations.reason];
    }
    media.push_back(
        {.row = std::move(row), .conversations = std::move(conversations)});
  }

  std::unordered_map<std::string, std::vector<std::size_t>> album_groups;
  for (std::size_t index = 0; index < media.size(); ++index) {
    const auto &mapping = media[index];
    if (!mapping.conversations.resolved) {
      continue;
    }
    const auto key = db_string(mapping.row, "source_installation") + '\x1f' +
                     db_string(mapping.row, "source_platform") + '\x1f' +
                     mapping.conversations.source + '\x1f' +
                     db_string(mapping.row, "media_group_id") + '\x1f' +
                     db_string(mapping.row, "target_installation") + '\x1f' +
                     db_string(mapping.row, "target_platform") + '\x1f' +
                     mapping.conversations.target + '\x1f' +
                     db_string(mapping.row, "target_message_id");
    album_groups[key].push_back(index);
  }
  for (const auto &[_, indices] : album_groups) {
    if (indices.size() == 1) {
      media[indices.front()].primary = true;
      continue;
    }
    std::size_t primary_count = 0;
    std::size_t primary_index = 0;
    for (const auto index : indices) {
      if (media[index].conversations.primary_evidence) {
        ++primary_count;
        primary_index = index;
      }
    }
    if (primary_count == 1) {
      for (const auto index : indices) {
        media[index].primary = index == primary_index;
      }
      continue;
    }
    for (const auto index : indices) {
      media[index].conversations.resolved = false;
      media[index].conversations.reason = "album_primary_unresolved";
      ++reasons["album_primary_unresolved"];
    }
  }

  for (const auto &row :
       connection.query("SELECT * FROM bridge_message_retry_queue_obcx_v2;")) {
    const auto source_group = db_optional_string(row, "source_group_id");
    const auto target_group = db_string(row, "group_id");
    if (!source_group.has_value() || source_group->empty() ||
        target_group.empty()) {
      ++reasons["retry_conversation_missing"];
      continue;
    }
    try {
      const auto source_conversation = canonical_conversation_id(
          db_string(row, "source_platform"), *source_group);
      const auto target_conversation = canonical_conversation_id(
          db_string(row, "target_platform"), target_group);
      connection.execute(
          R"(
        INSERT INTO bridge_message_retry_queue
          (id, source_installation, source_platform, source_conversation_id,
           target_installation, target_platform, target_conversation_id,
           source_message_id, message_content, group_id, source_group_id,
           target_topic_id, retry_count, max_retry_count, failure_reason,
           retry_type, next_retry_at, created_at, last_attempt_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
      )",
          {db_int64(row, "id"), db_string(row, "source_installation"),
           db_string(row, "source_platform"), source_conversation,
           db_string(row, "target_installation"),
           db_string(row, "target_platform"), target_conversation,
           db_string(row, "source_message_id"),
           db_string(row, "message_content"), target_group, *source_group,
           db_int64(row, "target_topic_id"), db_int64(row, "retry_count"),
           db_int64(row, "max_retry_count"),
           optional_string(db_optional_string(row, "failure_reason")),
           db_string(row, "retry_type"), db_int64(row, "next_retry_at"),
           db_int64(row, "created_at"), db_int64(row, "last_attempt_at")});
    } catch (const std::exception &) {
      ++reasons["retry_conversation_invalid"];
    }
  }

  const auto unresolved_mappings =
      std::ranges::count_if(mappings, [](const auto &entry) {
        return !entry.conversations.resolved;
      });
  const auto unresolved_media = std::ranges::count_if(
      media, [](const auto &entry) { return !entry.conversations.resolved; });
  const auto migrated_retries =
      table_row_count(connection, std::string{kRetriesTable});
  if (migrated_retries != retry_count) {
    if (migration.unresolved_mapping_policy ==
        LegacyUnresolvedMappingPolicy::Archive) {
      throw std::runtime_error(
          "bridge schema 2 -> 3 cannot archive unresolved retries");
    }
  }
  if ((!reasons.empty() || unresolved_mappings != 0 || unresolved_media != 0 ||
       migrated_retries != retry_count) &&
      migration.unresolved_mapping_policy ==
          LegacyUnresolvedMappingPolicy::Fail) {
    throw std::runtime_error(migration_failure_summary(reasons));
  }

  if ((unresolved_mappings != 0 || unresolved_media != 0) &&
      migration.unresolved_mapping_policy ==
          LegacyUnresolvedMappingPolicy::Archive) {
    create_v2_archives(connection);
  }

  for (const auto &mapping : mappings) {
    const auto &row = mapping.row;
    if (!mapping.conversations.resolved) {
      connection.execute(
          R"(
        INSERT INTO bridge_message_mappings_v2_archive
          (original_id, source_installation, source_platform,
           source_message_id, target_installation, target_platform,
           target_message_id, created_at, reason)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
      )",
          {db_int64(row, "id"), db_string(row, "source_installation"),
           db_string(row, "source_platform"),
           db_string(row, "source_message_id"),
           db_string(row, "target_installation"),
           db_string(row, "target_platform"),
           db_string(row, "target_message_id"), db_int64(row, "created_at"),
           mapping.conversations.reason});
      continue;
    }
    connection.execute(
        R"(
      INSERT INTO bridge_message_mappings
        (id, source_installation, source_platform, source_conversation_id,
         source_message_id, target_installation, target_platform,
         target_conversation_id, target_message_id, is_primary, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )",
        {db_int64(row, "id"), db_string(row, "source_installation"),
         db_string(row, "source_platform"), mapping.conversations.source,
         db_string(row, "source_message_id"),
         db_string(row, "target_installation"),
         db_string(row, "target_platform"), mapping.conversations.target,
         db_string(row, "target_message_id"),
         static_cast<std::int64_t>(mapping.primary ? 1 : 0),
         db_int64(row, "created_at")});
  }

  for (const auto &mapping : media) {
    const auto &row = mapping.row;
    if (!mapping.conversations.resolved) {
      connection.execute(
          R"(
        INSERT INTO bridge_media_group_mappings_v2_archive
          (original_id, source_installation, source_platform, media_group_id,
           source_message_id, target_installation, target_platform,
           target_message_id, target_group_id, created_at, reason)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
      )",
          {db_int64(row, "id"), db_string(row, "source_installation"),
           db_string(row, "source_platform"), db_string(row, "media_group_id"),
           db_string(row, "source_message_id"),
           db_string(row, "target_installation"),
           db_string(row, "target_platform"),
           db_string(row, "target_message_id"),
           db_string(row, "target_group_id"), db_int64(row, "created_at"),
           mapping.conversations.reason});
      continue;
    }
    connection.execute(
        R"(
      INSERT INTO bridge_media_group_mappings
        (id, source_installation, source_platform, source_conversation_id,
         media_group_id, source_message_id, target_installation,
         target_platform, target_conversation_id, target_message_id,
         target_group_id, is_primary, created_at)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )",
        {db_int64(row, "id"), db_string(row, "source_installation"),
         db_string(row, "source_platform"), mapping.conversations.source,
         db_string(row, "media_group_id"), db_string(row, "source_message_id"),
         db_string(row, "target_installation"),
         db_string(row, "target_platform"), mapping.conversations.target,
         db_string(row, "target_message_id"), db_string(row, "target_group_id"),
         static_cast<std::int64_t>(mapping.primary ? 1 : 0),
         db_int64(row, "created_at")});
  }

  const auto archived_mappings =
      table_row_count(connection, std::string{kMappingsArchiveTable});
  const auto archived_media =
      table_row_count(connection, std::string{kMediaGroupsArchiveTable});
  if (table_row_count(connection, std::string{kMappingsTable}) +
              archived_mappings !=
          mapping_count ||
      table_row_count(connection, std::string{kMediaGroupsTable}) +
              archived_media !=
          media_count ||
      table_row_count(connection, std::string{kRetriesTable}) != retry_count) {
    throw std::runtime_error(
        "bridge schema 2 -> 3 row count verification failed");
  }

  connection.execute("DROP TABLE bridge_message_mappings_obcx_v2;");
  connection.execute("DROP TABLE bridge_message_retry_queue_obcx_v2;");
  connection.execute("DROP TABLE bridge_media_group_mappings_obcx_v2;");
  connection.execute("DROP TABLE bridge_v3_message_sources;");
  connection.execute("DROP TABLE bridge_v3_source_keys;");
  validate_v3_shape(connection);
  OBCX_INFO(
      "Bridge state schema migration complete: version=3, mappings={}, "
      "retries={}, media_groups={}, archived_mappings={}, archived_media={}",
      mapping_count - archived_mappings, retry_count,
      media_count - archived_media, archived_mappings, archived_media);
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

void BridgeStateRepository::initialize_schema(
    std::optional<BridgeStateMigrationContext> migration) {
  db_manager_.with_migration_lock(
      db_instance_, db_namespace_,
      [migration = std::move(migration)](
          obcx::core::IDbConnection &connection) mutable {
        std::int64_t version = 0;
        bool fresh_state = false;
        const bool versioned = table_exists(connection, kSchemaTable);
        if (versioned) {
          const auto rows = connection.query(
              "SELECT MAX(version) AS version FROM bridge_schema_version;");
          version = rows.empty() ? 0 : db_int64(rows.front(), "version");
          if (version > BridgeStateRepository::current_schema_version) {
            throw std::runtime_error(
                "bridge schema version is newer than this binary");
          }
          if (version == BridgeStateRepository::current_schema_version) {
            validate_v3_shape(connection);
            return;
          }
          if (version != 2) {
            throw std::runtime_error("bridge schema version is invalid");
          }
          if (migration && !migration->allow_legacy_migration) {
            throw BridgeSchemaMigrationRequiresRestart{};
          }
          validate_v2_shape(connection);
        }

        BridgeStateMigrationContext resolved;
        if (migration) {
          resolved = *migration;
        }

        if (!versioned) {
          std::unordered_map<std::string, std::int64_t> legacy_counts;
          std::int64_t total_rows = 0;
          bool has_legacy_tables = false;
          for (const auto &table : kStateTables) {
            has_legacy_tables =
                has_legacy_tables || table_exists(connection, table);
            const auto count = table_row_count(connection, table);
            legacy_counts.emplace(table, count);
            total_rows += count;
          }

          if (has_legacy_tables) {
            if (migration && !migration->allow_legacy_migration) {
              throw BridgeSchemaMigrationRequiresRestart{};
            }
            if (total_rows > 0 && (!migration || migration->pair_id.empty() ||
                                   migration->telegram_installation.empty() ||
                                   migration->onebot11_installation.empty())) {
              throw std::runtime_error(
                  "bridge non-empty version-1 state requires a deterministic "
                  "legacy installation pair");
            }
            if (!migration) {
              resolved = {.pair_id = "empty",
                          .telegram_installation = "unused-telegram",
                          .onebot11_installation = "unused-onebot"};
            }
            OBCX_INFO("Migrating Bridge state schema 1 -> 2: pair={}, rows={}",
                      resolved.pair_id, total_rows);
            migrate_v1_tables(connection, resolved, legacy_counts);
            OBCX_INFO("Bridge state intermediate schema ready: version=2, "
                      "rows={}",
                      total_rows);
          } else {
            create_v2_tables(connection);
            fresh_state = true;
          }
          version = 2;
        }

        if (!resolved.allow_legacy_migration && !fresh_state) {
          throw BridgeSchemaMigrationRequiresRestart{};
        }
        OBCX_INFO("Migrating Bridge state schema 2 -> 3: policy={}, routes={}",
                  resolved.unresolved_mapping_policy ==
                          LegacyUnresolvedMappingPolicy::Archive
                      ? "archive"
                      : "fail",
                  resolved.conversation_routes.size());
        const auto migration_started = std::chrono::steady_clock::now();
        migrate_v2_message_state(connection, resolved);
        const auto migration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - migration_started)
                .count();
        OBCX_INFO("Bridge state schema 2 -> 3 transaction prepared in {} ms",
                  migration_ms);

        if (!versioned) {
          connection.execute(
              "CREATE TABLE bridge_schema_version (version INTEGER NOT NULL);");
          connection.execute("INSERT INTO bridge_schema_version(version) "
                             "VALUES (?);",
                             {BridgeStateRepository::current_schema_version});
        } else {
          connection.execute("DELETE FROM bridge_schema_version;");
          connection.execute("INSERT INTO bridge_schema_version(version) "
                             "VALUES (?);",
                             {BridgeStateRepository::current_schema_version});
        }
        validate_v3_shape(connection);
      });
}

void BridgeStateRepository::validate_schema() const {
  db_manager_.run_read<void>(
      db_instance_, [](obcx::core::IDbConnection &connection) {
        if (!table_exists(connection, kSchemaTable)) {
          throw BridgeSchemaMigrationRequiresRestart{};
        }
        const auto rows = connection.query(
            "SELECT MAX(version) AS version FROM bridge_schema_version;");
        if (rows.empty()) {
          throw std::runtime_error("bridge schema version is invalid");
        }
        const auto version = db_int64(rows.front(), "version");
        if (version < BridgeStateRepository::current_schema_version) {
          throw BridgeSchemaMigrationRequiresRestart{};
        }
        if (version > BridgeStateRepository::current_schema_version) {
          throw std::runtime_error(
              "bridge schema version is newer than this binary");
        }
        validate_v3_shape(connection);
      });
}

auto BridgeStateRepository::schema_version() const -> std::int64_t {
  return db_manager_.run_read<std::int64_t>(
      db_instance_, [](obcx::core::IDbConnection &connection) {
        if (table_exists(connection, kSchemaTable)) {
          const auto rows = connection.query(
              "SELECT MAX(version) AS version FROM bridge_schema_version;");
          return rows.empty() ? 0 : db_int64(rows.front(), "version");
        }
        return table_exists(connection, kMappingsTable) ? std::int64_t{1}
                                                        : std::int64_t{0};
      });
}

namespace {

auto mapping_from_row(const obcx::core::DbRow &row) -> storage::MessageMapping {
  return {.source_installation = db_string(row, "source_installation"),
          .source_platform = db_string(row, "source_platform"),
          .source_conversation_id = db_string(row, "source_conversation_id"),
          .source_message_id = db_string(row, "source_message_id"),
          .target_installation = db_string(row, "target_installation"),
          .target_platform = db_string(row, "target_platform"),
          .target_conversation_id = db_string(row, "target_conversation_id"),
          .target_message_id = db_string(row, "target_message_id"),
          .is_primary = db_int64(row, "is_primary") != 0,
          .created_at = time_point_from_ms(db_int64(row, "created_at"))};
}

} // namespace

auto BridgeStateRepository::add_message_mapping(
    const storage::MessageMapping &mapping,
    const MessageMappingWritePurpose purpose) -> bool {
  validate_message_identity({.installation_id = mapping.source_installation,
                             .platform = mapping.source_platform,
                             .conversation_id = mapping.source_conversation_id,
                             .message_id = mapping.source_message_id},
                            "source mapping");
  validate_message_identity({.installation_id = mapping.target_installation,
                             .platform = mapping.target_platform,
                             .conversation_id = mapping.target_conversation_id,
                             .message_id = mapping.target_message_id},
                            "target mapping");
  if (mapping.source_platform == mapping.target_platform) {
    throw std::invalid_argument(
        "Bridge mapping source and target platforms must differ");
  }
  switch (purpose) {
  case MessageMappingWritePurpose::General:
    general_mapping_writes_.fetch_add(1, std::memory_order_relaxed);
    break;
  case MessageMappingWritePurpose::DirectForward:
    direct_forward_writes_.fetch_add(1, std::memory_order_relaxed);
    break;
  case MessageMappingWritePurpose::RetryCompletion:
    retry_completion_writes_.fetch_add(1, std::memory_order_relaxed);
    break;
  case MessageMappingWritePurpose::DeferredMediaGroup:
    deferred_media_group_writes_.fetch_add(1, std::memory_order_relaxed);
    break;
  }
  return db_manager_.run_write<bool>(
      db_instance_, [&mapping](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_message_mappings
            (source_installation, source_platform, source_conversation_id,
             source_message_id, target_installation, target_platform,
             target_conversation_id, target_message_id, is_primary, created_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(source_installation, source_platform,
                      source_conversation_id, source_message_id,
                      target_installation, target_platform,
                      target_conversation_id)
          DO UPDATE SET target_message_id = excluded.target_message_id,
                        is_primary = excluded.is_primary,
                        created_at = excluded.created_at;
        )",
            {mapping.source_installation, mapping.source_platform,
             mapping.source_conversation_id, mapping.source_message_id,
             mapping.target_installation, mapping.target_platform,
             mapping.target_conversation_id, mapping.target_message_id,
             static_cast<std::int64_t>(mapping.is_primary ? 1 : 0),
             timestamp_ms(mapping.created_at)});
        return true;
      });
}

auto BridgeStateRepository::resolve_target_mapping(
    const BridgeMessageIdentity &source, const BridgeMessageScope &target,
    const MessageMappingReadPurpose purpose) -> MessageMappingResolution {
  validate_message_identity(source, "source lookup");
  validate_message_scope(target, "target lookup");
  switch (purpose) {
  case MessageMappingReadPurpose::General:
    general_mapping_reads_.fetch_add(1, std::memory_order_relaxed);
    break;
  case MessageMappingReadPurpose::PreSendDeduplication:
    pre_send_deduplication_reads_.fetch_add(1, std::memory_order_relaxed);
    break;
  case MessageMappingReadPurpose::PostSendRecovery:
    post_send_recovery_reads_.fetch_add(1, std::memory_order_relaxed);
    break;
  }
  return db_manager_.run_read<MessageMappingResolution>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT * FROM bridge_message_mappings
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND source_message_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?;
        )",
            {source.installation_id, source.platform, source.conversation_id,
             source.message_id, target.installation_id, target.platform,
             target.conversation_id});
        if (rows.empty()) {
          return MessageMappingResolution{};
        }
        if (rows.size() != 1) {
          return MessageMappingResolution{
              .status = MappingResolutionStatus::Corrupt,
              .diagnostic = "corrupt_message_mapping"};
        }
        return MessageMappingResolution{
            .status = MappingResolutionStatus::Unique,
            .mapping = mapping_from_row(rows.front())};
      });
}

auto BridgeStateRepository::resolve_source_mapping(
    const BridgeMessageIdentity &target, const BridgeMessageScope &source)
    -> MessageMappingResolution {
  validate_message_identity(target, "target reverse lookup");
  validate_message_scope(source, "source reverse lookup");
  general_mapping_reads_.fetch_add(1, std::memory_order_relaxed);
  return db_manager_.run_read<MessageMappingResolution>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT * FROM bridge_message_mappings
          WHERE target_installation = ? AND target_platform = ?
            AND target_conversation_id = ? AND target_message_id = ?
            AND source_installation = ? AND source_platform = ?
            AND source_conversation_id = ?;
        )",
            {target.installation_id, target.platform, target.conversation_id,
             target.message_id, source.installation_id, source.platform,
             source.conversation_id});
        if (rows.empty()) {
          return MessageMappingResolution{};
        }
        std::optional<storage::MessageMapping> primary;
        std::size_t primary_count = 0;
        for (const auto &row : rows) {
          auto mapping = mapping_from_row(row);
          if (mapping.is_primary) {
            ++primary_count;
            primary = std::move(mapping);
          }
        }
        if (primary_count == 1) {
          return MessageMappingResolution{.status =
                                              MappingResolutionStatus::Unique,
                                          .mapping = std::move(primary)};
        }
        return MessageMappingResolution{
            .status = primary_count == 0 ? MappingResolutionStatus::Corrupt
                                         : MappingResolutionStatus::Ambiguous,
            .diagnostic = primary_count == 0 ? "corrupt_message_mapping"
                                             : "ambiguous_message_mapping"};
      });
}

auto BridgeStateRepository::message_mapping_operation_counts() const noexcept
    -> MessageMappingOperationCounts {
  return {
      .general_reads = general_mapping_reads_.load(std::memory_order_relaxed),
      .pre_send_deduplication_reads =
          pre_send_deduplication_reads_.load(std::memory_order_relaxed),
      .post_send_recovery_reads =
          post_send_recovery_reads_.load(std::memory_order_relaxed),
      .general_writes = general_mapping_writes_.load(std::memory_order_relaxed),
      .direct_forward_writes =
          direct_forward_writes_.load(std::memory_order_relaxed),
      .retry_completion_writes =
          retry_completion_writes_.load(std::memory_order_relaxed),
      .deferred_media_group_writes =
          deferred_media_group_writes_.load(std::memory_order_relaxed),
  };
}

void BridgeStateRepository::reset_message_mapping_operation_counts() noexcept {
  general_mapping_reads_.store(0, std::memory_order_relaxed);
  pre_send_deduplication_reads_.store(0, std::memory_order_relaxed);
  post_send_recovery_reads_.store(0, std::memory_order_relaxed);
  general_mapping_writes_.store(0, std::memory_order_relaxed);
  direct_forward_writes_.store(0, std::memory_order_relaxed);
  retry_completion_writes_.store(0, std::memory_order_relaxed);
  deferred_media_group_writes_.store(0, std::memory_order_relaxed);
}

auto BridgeStateRepository::update_message_mapping(
    const BridgeMessageIdentity &source, const BridgeMessageScope &target,
    const std::string &new_target_message_id) -> bool {
  validate_message_identity(source, "source mapping update");
  validate_message_scope(target, "target mapping update");
  if (new_target_message_id.empty()) {
    throw std::invalid_argument("Bridge target message id is empty");
  }
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_message_mappings
          SET target_message_id = ?, created_at = ?
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND source_message_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?;
        )",
                           {new_target_message_id,
                            timestamp_ms(std::chrono::system_clock::now()),
                            source.installation_id, source.platform,
                            source.conversation_id, source.message_id,
                            target.installation_id, target.platform,
                            target.conversation_id});
        return true;
      });
}

auto BridgeStateRepository::delete_message_mapping(
    const BridgeMessageIdentity &source, const BridgeMessageScope &target)
    -> bool {
  validate_message_identity(source, "source mapping delete");
  validate_message_scope(target, "target mapping delete");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          DELETE FROM bridge_message_mappings
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND source_message_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?;
        )",
                           {source.installation_id, source.platform,
                            source.conversation_id, source.message_id,
                            target.installation_id, target.platform,
                            target.conversation_id});
        return true;
      });
}

auto BridgeStateRepository::save_or_update_user(
    const storage::UserInfo &user_info, const bool force_update) -> bool {
  require_installation(user_info.installation_id, "user installation");
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
            (installation_id, platform, user_id, group_id, username, nickname,
             title, first_name, last_name, last_updated)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(installation_id, platform, user_id, group_id)
          DO UPDATE SET
            username = excluded.username,
            nickname = excluded.nickname,
            title = excluded.title,
            first_name = excluded.first_name,
            last_name = excluded.last_name,
            last_updated = excluded.last_updated
        )"} + update_guard + ";",
                           {user_info.installation_id, user_info.platform,
                            user_info.user_id, user_info.group_id,
                            user_info.username, user_info.nickname,
                            user_info.title, user_info.first_name,
                            user_info.last_name,
                            timestamp_ms(user_info.last_updated)});
        return true;
      });
}

auto BridgeStateRepository::get_user(const std::string &installation_id,
                                     const std::string &platform,
                                     const std::string &user_id,
                                     const std::string &group_id)
    -> std::optional<storage::UserInfo> {
  return db_manager_.run_read<std::optional<storage::UserInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows =
            connection.query(R"(
          SELECT installation_id, platform, user_id, group_id, username,
                 nickname, title, first_name, last_name, last_updated
          FROM bridge_users
          WHERE installation_id = ? AND platform = ? AND user_id = ?
            AND group_id = ?
          LIMIT 1;
        )",
                             {installation_id, platform, user_id, group_id});
        if (rows.empty()) {
          return std::optional<storage::UserInfo>{};
        }
        const auto &row = rows.front();
        return std::optional<storage::UserInfo>{storage::UserInfo{
            .installation_id = db_string(row, "installation_id"),
            .platform = db_string(row, "platform"),
            .user_id = db_string(row, "user_id"),
            .group_id = db_string(row, "group_id"),
            .username = db_string(row, "username"),
            .nickname = db_string(row, "nickname"),
            .title = db_string(row, "title"),
            .first_name = db_string(row, "first_name"),
            .last_name = db_string(row, "last_name"),
            .last_updated = time_point_from_ms(db_int64(row, "last_updated")),
        }};
      });
}

auto BridgeStateRepository::query_user_display_name(
    const std::string &installation_id, const std::string &platform,
    const std::string &user_id, const std::string &group_id)
    -> std::optional<std::string> {
  constexpr auto kUserInfoExpiration = std::chrono::minutes{10};
  const auto query_group_id = platform == "telegram" ? std::string{} : group_id;
  const auto user_info =
      get_user(installation_id, platform, user_id, query_group_id);
  if (!user_info.has_value() ||
      std::chrono::system_clock::now() - user_info->last_updated >
          kUserInfoExpiration) {
    return std::nullopt;
  }
  if (platform == "telegram") {
    if (!user_info->nickname.empty()) {
      return user_info->nickname;
    }
    if (!user_info->first_name.empty()) {
      auto display_name = user_info->first_name;
      if (!user_info->last_name.empty()) {
        display_name += " " + user_info->last_name;
      }
      return display_name;
    }
    if (!user_info->username.empty()) {
      return user_info->username;
    }
    return std::nullopt;
  }
  if (!user_info->nickname.empty()) {
    return user_info->nickname;
  }
  if (!user_info->username.empty()) {
    return user_info->username;
  }
  if (!user_info->first_name.empty()) {
    auto display_name = user_info->first_name;
    if (!user_info->last_name.empty()) {
      display_name += " " + user_info->last_name;
    }
    return display_name;
  }
  return std::nullopt;
}

auto BridgeStateRepository::save_sticker_cache(
    const storage::StickerCacheInfo &cache_info) -> bool {
  require_installation(cache_info.installation_id, "sticker installation");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_sticker_cache
            (installation_id, platform, sticker_id, sticker_hash,
             original_name, file_type, mime_type, original_file_path,
             converted_file_path, container_path, file_size, conversion_status,
             created_at, last_used_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(installation_id, platform, sticker_hash) DO UPDATE SET
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
            {cache_info.installation_id, cache_info.platform,
             cache_info.sticker_id, cache_info.sticker_hash,
             optional_string(cache_info.original_name), cache_info.file_type,
             optional_string(cache_info.mime_type),
             cache_info.original_file_path,
             optional_string(cache_info.converted_file_path),
             cache_info.container_path, optional_int64(cache_info.file_size),
             cache_info.conversion_status, timestamp_ms(cache_info.created_at),
             timestamp_ms(cache_info.last_used_at)});
        return true;
      });
}

auto BridgeStateRepository::get_sticker_cache(
    const std::string &installation_id, const std::string &platform,
    const std::string &sticker_hash)
    -> std::optional<storage::StickerCacheInfo> {
  return db_manager_.run_read<std::optional<storage::StickerCacheInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows =
            connection.query(R"(
          SELECT installation_id, platform, sticker_id, sticker_hash,
                 original_name, file_type, mime_type, original_file_path,
                 converted_file_path, container_path, file_size,
                 conversion_status, created_at, last_used_at
          FROM bridge_sticker_cache
          WHERE installation_id = ? AND platform = ? AND sticker_hash = ?
          LIMIT 1;
        )",
                             {installation_id, platform, sticker_hash});
        if (rows.empty()) {
          return std::optional<storage::StickerCacheInfo>{};
        }
        const auto &row = rows.front();
        storage::StickerCacheInfo result;
        result.installation_id = db_string(row, "installation_id");
        result.platform = db_string(row, "platform");
        result.sticker_id = db_string(row, "sticker_id");
        result.sticker_hash = db_string(row, "sticker_hash");
        result.original_name = db_optional_string(row, "original_name");
        result.file_type = db_string(row, "file_type");
        result.mime_type = db_optional_string(row, "mime_type");
        result.original_file_path = db_string(row, "original_file_path");
        result.converted_file_path =
            db_optional_string(row, "converted_file_path");
        result.container_path = db_string(row, "container_path");
        result.file_size = db_optional_int64(row, "file_size");
        result.conversion_status = db_string(row, "conversion_status");
        result.created_at = time_point_from_ms(db_int64(row, "created_at"));
        result.last_used_at = time_point_from_ms(db_int64(row, "last_used_at"));
        return std::optional<storage::StickerCacheInfo>{std::move(result)};
      });
}

auto BridgeStateRepository::update_sticker_last_used(
    const std::string &installation_id, const std::string &platform,
    const std::string &sticker_hash) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_sticker_cache SET last_used_at = ?
          WHERE installation_id = ? AND platform = ? AND sticker_hash = ?;
        )",
                           {timestamp_ms(std::chrono::system_clock::now()),
                            installation_id, platform, sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::update_sticker_conversion(
    const std::string &installation_id, const std::string &platform,
    const std::string &sticker_hash, const std::string &conversion_status,
    const std::optional<std::string> &converted_file_path) -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_sticker_cache
          SET conversion_status = ?, converted_file_path = ?
          WHERE installation_id = ? AND platform = ? AND sticker_hash = ?;
        )",
                           {conversion_status,
                            optional_string(converted_file_path),
                            installation_id, platform, sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::save_qq_sticker_mapping(
    const storage::QQStickerMapping &mapping) -> bool {
  require_installation(mapping.source_installation,
                       "QQ sticker source installation");
  require_installation(mapping.target_installation,
                       "QQ sticker target installation");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_qq_sticker_mappings
            (source_installation, target_installation, qq_sticker_hash,
             telegram_file_id, file_type, created_at, last_used_at, is_gif,
             content_type, last_checked_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(source_installation, target_installation, qq_sticker_hash)
          DO UPDATE SET
            telegram_file_id = excluded.telegram_file_id,
            file_type = excluded.file_type,
            created_at = excluded.created_at,
            last_used_at = excluded.last_used_at,
            is_gif = excluded.is_gif,
            content_type = excluded.content_type,
            last_checked_at = excluded.last_checked_at;
        )",
            {mapping.source_installation, mapping.target_installation,
             mapping.qq_sticker_hash, mapping.telegram_file_id,
             mapping.file_type, timestamp_ms(mapping.created_at),
             timestamp_ms(mapping.last_used_at), optional_bool(mapping.is_gif),
             optional_string(mapping.content_type),
             optional_time_ms(mapping.last_checked_at)});
        return true;
      });
}

auto BridgeStateRepository::get_qq_sticker_mapping(
    const std::string &source_installation,
    const std::string &target_installation, const std::string &qq_sticker_hash)
    -> std::optional<storage::QQStickerMapping> {
  return db_manager_.run_read<std::optional<storage::QQStickerMapping>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT source_installation, target_installation, qq_sticker_hash,
                 telegram_file_id, file_type, created_at, last_used_at, is_gif,
                 content_type, last_checked_at
          FROM bridge_qq_sticker_mappings
          WHERE source_installation = ? AND target_installation = ?
            AND qq_sticker_hash = ?
          LIMIT 1;
        )",
            {source_installation, target_installation, qq_sticker_hash});
        if (rows.empty()) {
          return std::optional<storage::QQStickerMapping>{};
        }
        const auto &row = rows.front();
        return std::optional<storage::QQStickerMapping>{
            storage::QQStickerMapping{
                .source_installation = db_string(row, "source_installation"),
                .target_installation = db_string(row, "target_installation"),
                .qq_sticker_hash = db_string(row, "qq_sticker_hash"),
                .telegram_file_id = db_string(row, "telegram_file_id"),
                .file_type = db_string(row, "file_type"),
                .created_at = time_point_from_ms(db_int64(row, "created_at")),
                .last_used_at =
                    time_point_from_ms(db_int64(row, "last_used_at")),
                .is_gif = db_optional_bool(row, "is_gif"),
                .content_type = db_optional_string(row, "content_type"),
                .last_checked_at = db_optional_time(row, "last_checked_at"),
            }};
      });
}

auto BridgeStateRepository::update_qq_sticker_last_used(
    const std::string &source_installation,
    const std::string &target_installation, const std::string &qq_sticker_hash)
    -> bool {
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_qq_sticker_mappings SET last_used_at = ?
          WHERE source_installation = ? AND target_installation = ?
            AND qq_sticker_hash = ?;
        )",
                           {timestamp_ms(std::chrono::system_clock::now()),
                            source_installation, target_installation,
                            qq_sticker_hash});
        return true;
      });
}

auto BridgeStateRepository::add_message_retry(
    const storage::MessageRetryInfo &retry_info) -> bool {
  const BridgeMessageIdentity source{
      .installation_id = retry_info.source_installation,
      .platform = retry_info.source_platform,
      .conversation_id = retry_info.source_conversation_id,
      .message_id = retry_info.source_message_id};
  const BridgeMessageScope target{
      .installation_id = retry_info.target_installation,
      .platform = retry_info.target_platform,
      .conversation_id = retry_info.target_conversation_id};
  validate_message_identity(source, "retry source");
  validate_message_scope(target, "retry target");
  if (native_conversation_id(source.platform, source.conversation_id) !=
          retry_info.source_group_id ||
      native_conversation_id(target.platform, target.conversation_id) !=
          retry_info.group_id) {
    throw std::invalid_argument(
        "Bridge retry group metadata disagrees with conversation identity");
  }
  return db_manager_.run_write<bool>(
      db_instance_, [&retry_info](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_message_retry_queue
            (source_installation, source_platform, source_conversation_id,
             target_installation, target_platform, target_conversation_id,
             source_message_id, message_content, group_id, source_group_id,
             target_topic_id, retry_count, max_retry_count, failure_reason,
             retry_type, next_retry_at, created_at, last_attempt_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(source_installation, source_platform,
                      source_conversation_id, source_message_id,
                      target_installation, target_platform,
                      target_conversation_id)
          DO UPDATE SET message_content = excluded.message_content,
                        group_id = excluded.group_id,
                        source_group_id = excluded.source_group_id,
                        target_topic_id = excluded.target_topic_id,
                        retry_count = excluded.retry_count,
                        max_retry_count = excluded.max_retry_count,
                        failure_reason = excluded.failure_reason,
                        retry_type = excluded.retry_type,
                        next_retry_at = excluded.next_retry_at,
                        last_attempt_at = excluded.last_attempt_at;
        )",
            {retry_info.source_installation, retry_info.source_platform,
             retry_info.source_conversation_id, retry_info.target_installation,
             retry_info.target_platform, retry_info.target_conversation_id,
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
          SELECT source_installation, source_platform, source_conversation_id,
                 target_installation, target_platform, target_conversation_id,
                 source_message_id, message_content, group_id,
                 source_group_id, target_topic_id, retry_count,
                 max_retry_count, failure_reason, retry_type, next_retry_at,
                 created_at, last_attempt_at
          FROM bridge_message_retry_queue
          WHERE next_retry_at <= ? AND retry_count < max_retry_count
          ORDER BY next_retry_at ASC LIMIT ?;
        )",
            {timestamp_ms(ready_at), static_cast<std::int64_t>(limit)});
        std::vector<storage::MessageRetryInfo> retries;
        retries.reserve(rows.size());
        for (const auto &row : rows) {
          retries.push_back(storage::MessageRetryInfo{
              .source_installation = db_string(row, "source_installation"),
              .source_platform = db_string(row, "source_platform"),
              .source_conversation_id =
                  db_string(row, "source_conversation_id"),
              .target_installation = db_string(row, "target_installation"),
              .target_platform = db_string(row, "target_platform"),
              .target_conversation_id =
                  db_string(row, "target_conversation_id"),
              .source_message_id = db_string(row, "source_message_id"),
              .message_content = db_string(row, "message_content"),
              .group_id = db_string(row, "group_id"),
              .source_group_id = db_string(row, "source_group_id"),
              .target_topic_id = db_int64(row, "target_topic_id"),
              .retry_count = static_cast<int>(db_int64(row, "retry_count")),
              .max_retry_count =
                  static_cast<int>(db_int64(row, "max_retry_count")),
              .failure_reason =
                  db_optional_string(row, "failure_reason").value_or(""),
              .retry_type = db_string(row, "retry_type"),
              .next_retry_at =
                  time_point_from_ms(db_int64(row, "next_retry_at")),
              .created_at = time_point_from_ms(db_int64(row, "created_at")),
              .last_attempt_at =
                  time_point_from_ms(db_int64(row, "last_attempt_at")),
          });
        }
        return retries;
      });
}

auto BridgeStateRepository::update_message_retry(
    const BridgeMessageIdentity &source, const BridgeMessageScope &target,
    const int retry_count,
    const std::chrono::system_clock::time_point &next_retry_at,
    const std::string &failure_reason) -> bool {
  validate_message_identity(source, "retry source update");
  validate_message_scope(target, "retry target update");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          UPDATE bridge_message_retry_queue
          SET retry_count = ?, next_retry_at = ?, failure_reason = ?,
              last_attempt_at = ?
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND source_message_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?;
        )",
                           {static_cast<std::int64_t>(retry_count),
                            timestamp_ms(next_retry_at), failure_reason,
                            timestamp_ms(std::chrono::system_clock::now()),
                            source.installation_id, source.platform,
                            source.conversation_id, source.message_id,
                            target.installation_id, target.platform,
                            target.conversation_id});
        return true;
      });
}

auto BridgeStateRepository::remove_message_retry(
    const BridgeMessageIdentity &source, const BridgeMessageScope &target)
    -> bool {
  validate_message_identity(source, "retry source delete");
  validate_message_scope(target, "retry target delete");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          DELETE FROM bridge_message_retry_queue
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND source_message_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?;
        )",
                           {source.installation_id, source.platform,
                            source.conversation_id, source.message_id,
                            target.installation_id, target.platform,
                            target.conversation_id});
        return true;
      });
}

auto BridgeStateRepository::add_media_group_mapping(
    const MediaGroupMapping &mapping) -> bool {
  const BridgeMessageIdentity source{
      .installation_id = mapping.source_installation,
      .platform = mapping.source_platform,
      .conversation_id = mapping.source_conversation_id,
      .message_id = mapping.source_message_id};
  const BridgeMessageIdentity target{
      .installation_id = mapping.target_installation,
      .platform = mapping.target_platform,
      .conversation_id = mapping.target_conversation_id,
      .message_id = mapping.target_message_id};
  validate_message_identity(source, "media-group source");
  validate_message_identity(target, "media-group target");
  if (mapping.media_group_id.empty() ||
      native_conversation_id(target.platform, target.conversation_id) !=
          mapping.target_group_id) {
    throw std::invalid_argument(
        "Bridge media-group identity or target metadata is invalid");
  }
  return db_manager_.run_write<bool>(
      db_instance_, [&mapping](obcx::core::IDbConnection &connection) {
        connection.execute(
            R"(
          INSERT INTO bridge_media_group_mappings
            (source_installation, source_platform, source_conversation_id,
             media_group_id, source_message_id, target_installation,
             target_platform, target_conversation_id, target_message_id,
             target_group_id, is_primary, created_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
          ON CONFLICT(source_installation, source_platform,
                      source_conversation_id, media_group_id,
                      source_message_id, target_installation,
                      target_platform, target_conversation_id)
          DO UPDATE SET target_message_id = excluded.target_message_id,
                        target_group_id = excluded.target_group_id,
                        is_primary = excluded.is_primary,
                        created_at = excluded.created_at;
        )",
            {mapping.source_installation, mapping.source_platform,
             mapping.source_conversation_id, mapping.media_group_id,
             mapping.source_message_id, mapping.target_installation,
             mapping.target_platform, mapping.target_conversation_id,
             mapping.target_message_id, mapping.target_group_id,
             static_cast<std::int64_t>(mapping.is_primary ? 1 : 0),
             timestamp_ms(mapping.created_at)});
        return true;
      });
}

auto BridgeStateRepository::get_media_group_mappings(
    const BridgeMessageScope &source, const std::string &media_group_id,
    const BridgeMessageScope &target) -> std::vector<MediaGroupMapping> {
  validate_message_scope(source, "media-group source lookup");
  validate_message_scope(target, "media-group target lookup");
  return db_manager_.run_read<std::vector<MediaGroupMapping>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            R"(
          SELECT source_installation, source_platform, source_conversation_id,
                 media_group_id, source_message_id, target_installation,
                 target_platform, target_conversation_id, target_message_id,
                 target_group_id, is_primary, created_at
          FROM bridge_media_group_mappings
          WHERE source_installation = ? AND source_platform = ?
            AND source_conversation_id = ? AND media_group_id = ?
            AND target_installation = ? AND target_platform = ?
            AND target_conversation_id = ?
          ORDER BY is_primary DESC, source_message_id ASC;
        )",
            {source.installation_id, source.platform, source.conversation_id,
             media_group_id, target.installation_id, target.platform,
             target.conversation_id});
        std::vector<MediaGroupMapping> mappings;
        mappings.reserve(rows.size());
        for (const auto &row : rows) {
          mappings.push_back(MediaGroupMapping{
              .source_installation = db_string(row, "source_installation"),
              .source_platform = db_string(row, "source_platform"),
              .source_conversation_id =
                  db_string(row, "source_conversation_id"),
              .media_group_id = db_string(row, "media_group_id"),
              .source_message_id = db_string(row, "source_message_id"),
              .target_installation = db_string(row, "target_installation"),
              .target_platform = db_string(row, "target_platform"),
              .target_conversation_id =
                  db_string(row, "target_conversation_id"),
              .target_message_id = db_string(row, "target_message_id"),
              .target_group_id = db_string(row, "target_group_id"),
              .is_primary = db_int64(row, "is_primary") != 0,
              .created_at = time_point_from_ms(db_int64(row, "created_at")),
          });
        }
        return mappings;
      });
}

auto BridgeStateRepository::update_platform_heartbeat(
    const std::string &installation_id, const std::string &platform,
    const std::chrono::system_clock::time_point &heartbeat_time) -> bool {
  require_installation(installation_id, "heartbeat installation");
  return db_manager_.run_write<bool>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        connection.execute(R"(
          INSERT INTO bridge_platform_heartbeats
            (installation_id, platform, last_heartbeat_at, updated_at)
          VALUES (?, ?, ?, ?)
          ON CONFLICT(installation_id) DO UPDATE SET
            platform = excluded.platform,
            last_heartbeat_at = excluded.last_heartbeat_at,
            updated_at = excluded.updated_at;
        )",
                           {installation_id, platform,
                            timestamp_ms(heartbeat_time),
                            timestamp_ms(std::chrono::system_clock::now())});
        return true;
      });
}

auto BridgeStateRepository::get_platform_heartbeat(
    const std::string &installation_id)
    -> std::optional<storage::PlatformHeartbeatInfo> {
  return db_manager_.run_read<std::optional<storage::PlatformHeartbeatInfo>>(
      db_instance_, [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(R"(
          SELECT installation_id, platform, last_heartbeat_at, updated_at
          FROM bridge_platform_heartbeats
          WHERE installation_id = ? LIMIT 1;
        )",
                                           {installation_id});
        if (rows.empty()) {
          return std::optional<storage::PlatformHeartbeatInfo>{};
        }
        const auto &row = rows.front();
        return std::optional<storage::PlatformHeartbeatInfo>{
            storage::PlatformHeartbeatInfo{
                .installation_id = db_string(row, "installation_id"),
                .platform = db_string(row, "platform"),
                .last_heartbeat_at =
                    time_point_from_ms(db_int64(row, "last_heartbeat_at")),
                .updated_at = time_point_from_ms(db_int64(row, "updated_at")),
            }};
      });
}

} // namespace bridge

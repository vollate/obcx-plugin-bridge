#include "bridge_message_identity.hpp"
#include "bridge_state_repository.hpp"
#include "bridge_storage_models.hpp"
#include "common/config_loader.hpp"
#include "core/db_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr std::string_view kTgA = "tg-a";
constexpr std::string_view kQqA = "qq-a";
constexpr std::string_view kTgB = "tg-b";
constexpr std::string_view kQqB = "qq-b";

using bridge::BridgeMessageIdentity;
using bridge::BridgeMessageScope;
using bridge::BridgeStateMigrationContext;
using bridge::LegacyConversationRoute;
using bridge::LegacyUnresolvedMappingPolicy;

struct RouteIds {
  std::string telegram = "chat:tg-group";
  std::string qq = "group:qq-group";
  std::int64_t topic = -1;
};

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("obcx_bridge_" + name + "_" + std::to_string(stamp) + ".sqlite3");
}

auto sqlite_config(const std::string &name, const std::filesystem::path &path)
    -> obcx::common::DbInstanceConfig {
  obcx::common::DbInstanceConfig config;
  config.name = name;
  config.type = "sqlite";
  config.path = path.string();
  return config;
}

auto manager_for(const std::filesystem::path &path)
    -> std::shared_ptr<obcx::core::DbManager> {
  auto manager = std::make_shared<obcx::core::DbManager>();
  manager->configure({sqlite_config("main", path)});
  return manager;
}

auto table_exists(obcx::core::DbManager &manager, const std::string &table_name)
    -> bool {
  return manager.run_read<bool>(
      "main", [&](obcx::core::IDbConnection &connection) {
        return !connection
                    .query("SELECT name FROM sqlite_master WHERE type = "
                           "'table' AND name = ?;",
                           {table_name})
                    .empty();
      });
}

auto table_count(obcx::core::DbManager &manager, const std::string &table_name)
    -> std::int64_t {
  return manager.run_read<std::int64_t>(
      "main", [&](obcx::core::IDbConnection &connection) {
        if (connection
                .query("SELECT name FROM sqlite_master WHERE type='table' AND "
                       "name=?;",
                       {table_name})
                .empty()) {
          return std::int64_t{0};
        }
        return std::get<std::int64_t>(
            connection
                .query("SELECT COUNT(*) AS count FROM \"" + table_name + "\";")
                .front()
                .at("count"));
      });
}

auto index_exists(obcx::core::DbManager &manager, const std::string &index_name)
    -> bool {
  return manager.run_read<bool>(
      "main", [&](obcx::core::IDbConnection &connection) {
        return !connection
                    .query("SELECT name FROM sqlite_master WHERE type = "
                           "'index' AND name = ?;",
                           {index_name})
                    .empty();
      });
}

void execute_script(obcx::core::IDbConnection &connection,
                    const std::string_view script) {
  std::size_t begin = 0;
  while (begin < script.size()) {
    const auto end = script.find(';', begin);
    const auto length =
        end == std::string_view::npos ? script.size() - begin : end - begin + 1;
    const auto statement = script.substr(begin, length);
    if (statement.find_first_not_of(" \t\r\n;") != std::string_view::npos) {
      connection.execute(std::string{statement});
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
}

void create_message_store_tables(obcx::core::IDbConnection &connection) {
  for (const auto platform : {std::string{"qq"}, std::string{"telegram"}}) {
    execute_script(connection,
                   "CREATE TABLE message_store_" + platform + R"(_messages (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      message_id TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_bot TEXT NOT NULL DEFAULT '',
      conversation_id TEXT NOT NULL,
      sender TEXT NOT NULL DEFAULT '',
      group_id TEXT NOT NULL DEFAULT '',
      message_type TEXT NOT NULL DEFAULT 'unknown',
      payload TEXT NOT NULL DEFAULT '{}',
      raw TEXT NOT NULL DEFAULT '{}',
      timestamp INTEGER NOT NULL,
      created_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL,
      UNIQUE(source_platform, source_bot, conversation_id, message_id));
    )");
  }
}

void insert_stored_message(obcx::core::IDbConnection &connection,
                           const std::string &platform, const std::string &bot,
                           const std::string &conversation,
                           const std::string &message_id,
                           const std::string &payload = "{}") {
  connection.execute(
      "INSERT INTO message_store_" + platform +
          "_messages(message_id, source_platform, source_bot, "
          "conversation_id, payload, raw, timestamp, created_at, updated_at) "
          "VALUES (?, ?, ?, ?, ?, ?, 1, 1, 1);",
      {message_id, platform, bot, conversation, payload, payload});
}

void create_v2_tables(obcx::core::IDbConnection &connection) {
  execute_script(connection, R"(
    CREATE TABLE bridge_schema_version(version INTEGER NOT NULL);
    INSERT INTO bridge_schema_version VALUES (2);
    CREATE TABLE bridge_message_mappings (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      source_installation TEXT NOT NULL,
      source_platform TEXT NOT NULL,
      source_message_id TEXT NOT NULL,
      target_installation TEXT NOT NULL,
      target_platform TEXT NOT NULL,
      target_message_id TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      UNIQUE(source_installation, source_platform, source_message_id,
             target_installation, target_platform));
    CREATE TABLE bridge_message_retry_queue (
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
             target_installation, target_platform));
    CREATE INDEX idx_bridge_message_retry_next_retry
      ON bridge_message_retry_queue(next_retry_at);
    CREATE TABLE bridge_media_group_mappings (
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
             source_message_id, target_installation, target_platform));
    CREATE INDEX idx_bridge_media_group_lookup
      ON bridge_media_group_mappings(source_installation, source_platform,
                                     media_group_id, target_installation,
                                     target_platform);
    CREATE TABLE bridge_users (
      id INTEGER PRIMARY KEY AUTOINCREMENT, installation_id TEXT NOT NULL,
      platform TEXT NOT NULL, user_id TEXT NOT NULL,
      group_id TEXT NOT NULL DEFAULT '', username TEXT NOT NULL DEFAULT '',
      nickname TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',
      first_name TEXT NOT NULL DEFAULT '', last_name TEXT NOT NULL DEFAULT '',
      last_updated INTEGER NOT NULL,
      UNIQUE(installation_id, platform, user_id, group_id));
    CREATE TABLE bridge_sticker_cache (
      id INTEGER PRIMARY KEY AUTOINCREMENT, installation_id TEXT NOT NULL,
      platform TEXT NOT NULL, sticker_id TEXT NOT NULL,
      sticker_hash TEXT NOT NULL, original_name TEXT, file_type TEXT NOT NULL,
      mime_type TEXT, original_file_path TEXT NOT NULL,
      converted_file_path TEXT, container_path TEXT NOT NULL,
      file_size INTEGER, conversion_status TEXT NOT NULL,
      created_at INTEGER NOT NULL, last_used_at INTEGER NOT NULL,
      UNIQUE(installation_id, platform, sticker_hash));
    CREATE TABLE bridge_qq_sticker_mappings (
      source_installation TEXT NOT NULL, target_installation TEXT NOT NULL,
      qq_sticker_hash TEXT NOT NULL, telegram_file_id TEXT NOT NULL,
      file_type TEXT NOT NULL, created_at INTEGER NOT NULL,
      last_used_at INTEGER NOT NULL, is_gif INTEGER, content_type TEXT,
      last_checked_at INTEGER,
      PRIMARY KEY(source_installation, target_installation, qq_sticker_hash));
    CREATE TABLE bridge_platform_heartbeats (
      installation_id TEXT PRIMARY KEY NOT NULL, platform TEXT NOT NULL,
      last_heartbeat_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
  )");
}

auto route(std::string telegram_conversation = "chat:tg-group",
           std::string qq_conversation = "group:qq-group",
           std::string telegram_installation = std::string{kTgA},
           std::string onebot_installation = std::string{kQqA},
           std::int64_t topic = -1) -> LegacyConversationRoute {
  return {.pair_id = "primary",
          .telegram_installation = std::move(telegram_installation),
          .onebot11_installation = std::move(onebot_installation),
          .telegram_conversation_id = std::move(telegram_conversation),
          .telegram_topic_id = topic,
          .qq_conversation_id = std::move(qq_conversation)};
}

auto migration_context(std::vector<LegacyConversationRoute> routes = {route()},
                       const LegacyUnresolvedMappingPolicy policy =
                           LegacyUnresolvedMappingPolicy::Fail,
                       const bool allow = true) -> BridgeStateMigrationContext {
  return {.pair_id = "primary",
          .telegram_installation = std::string{kTgA},
          .onebot11_installation = std::string{kQqA},
          .conversation_routes = std::move(routes),
          .unresolved_mapping_policy = policy,
          .allow_legacy_migration = allow};
}

auto mapping(std::string source_installation, std::string source_platform,
             std::string source_conversation, std::string source_id,
             std::string target_installation, std::string target_platform,
             std::string target_conversation, std::string target_id,
             const bool primary = true) -> storage::MessageMapping {
  return {.source_installation = std::move(source_installation),
          .source_platform = std::move(source_platform),
          .source_conversation_id = std::move(source_conversation),
          .source_message_id = std::move(source_id),
          .target_installation = std::move(target_installation),
          .target_platform = std::move(target_platform),
          .target_conversation_id = std::move(target_conversation),
          .target_message_id = std::move(target_id),
          .is_primary = primary,
          .created_at = std::chrono::system_clock::now()};
}

auto target_id(bridge::BridgeStateRepository &repository,
               const BridgeMessageIdentity &source,
               const BridgeMessageScope &target,
               const bridge::MessageMappingReadPurpose purpose =
                   bridge::MessageMappingReadPurpose::General)
    -> std::optional<std::string> {
  const auto result =
      repository.resolve_target_mapping(source, target, purpose);
  return result.unique()
             ? std::optional<std::string>{result.mapping->target_message_id}
             : std::nullopt;
}

void create_complete_v1_fixture(obcx::core::DbManager &manager,
                                std::string mapping_source_platform = "qq") {
  manager.run_write<void>("main", [&](obcx::core::IDbConnection &connection) {
    execute_script(connection, R"(
      CREATE TABLE bridge_message_mappings (
        id INTEGER PRIMARY KEY AUTOINCREMENT, source_platform TEXT NOT NULL,
        source_message_id TEXT NOT NULL, target_platform TEXT NOT NULL,
        target_message_id TEXT NOT NULL, created_at INTEGER NOT NULL,
        UNIQUE(source_platform, source_message_id, target_platform));
      CREATE TABLE bridge_message_retry_queue (
        id INTEGER PRIMARY KEY AUTOINCREMENT, source_platform TEXT NOT NULL,
        target_platform TEXT NOT NULL, source_message_id TEXT NOT NULL,
        message_content TEXT NOT NULL, group_id TEXT NOT NULL,
        source_group_id TEXT, target_topic_id INTEGER DEFAULT -1,
        retry_count INTEGER NOT NULL DEFAULT 0,
        max_retry_count INTEGER NOT NULL DEFAULT 5, failure_reason TEXT,
        retry_type TEXT NOT NULL DEFAULT 'message_send',
        next_retry_at INTEGER NOT NULL, created_at INTEGER NOT NULL,
        last_attempt_at INTEGER NOT NULL,
        UNIQUE(source_platform, source_message_id, target_platform));
      CREATE TABLE bridge_media_group_mappings (
        id INTEGER PRIMARY KEY AUTOINCREMENT, source_platform TEXT NOT NULL,
        media_group_id TEXT NOT NULL, source_message_id TEXT NOT NULL,
        target_platform TEXT NOT NULL, target_message_id TEXT NOT NULL,
        target_group_id TEXT NOT NULL, created_at INTEGER NOT NULL,
        UNIQUE(source_platform, media_group_id, source_message_id,
               target_platform));
      CREATE INDEX idx_bridge_media_group_lookup
        ON bridge_media_group_mappings(source_platform, media_group_id,
                                       target_platform);
      CREATE TABLE bridge_users (
        id INTEGER PRIMARY KEY AUTOINCREMENT, platform TEXT NOT NULL,
        user_id TEXT NOT NULL, group_id TEXT NOT NULL DEFAULT '',
        username TEXT NOT NULL DEFAULT '', nickname TEXT NOT NULL DEFAULT '',
        title TEXT NOT NULL DEFAULT '', first_name TEXT NOT NULL DEFAULT '',
        last_name TEXT NOT NULL DEFAULT '', last_updated INTEGER NOT NULL,
        UNIQUE(platform, user_id, group_id));
      CREATE TABLE bridge_sticker_cache (
        id INTEGER PRIMARY KEY AUTOINCREMENT, platform TEXT NOT NULL,
        sticker_id TEXT NOT NULL, sticker_hash TEXT NOT NULL,
        original_name TEXT, file_type TEXT NOT NULL, mime_type TEXT,
        original_file_path TEXT NOT NULL, converted_file_path TEXT,
        container_path TEXT NOT NULL, file_size INTEGER,
        conversion_status TEXT NOT NULL, created_at INTEGER NOT NULL,
        last_used_at INTEGER NOT NULL, UNIQUE(platform, sticker_hash));
      CREATE TABLE bridge_qq_sticker_mappings (
        qq_sticker_hash TEXT PRIMARY KEY NOT NULL,
        telegram_file_id TEXT NOT NULL, file_type TEXT NOT NULL,
        created_at INTEGER NOT NULL, last_used_at INTEGER NOT NULL,
        is_gif INTEGER, content_type TEXT, last_checked_at INTEGER);
      CREATE TABLE bridge_platform_heartbeats (
        platform TEXT PRIMARY KEY NOT NULL,
        last_heartbeat_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
      CREATE INDEX idx_bridge_message_retry_next_retry
        ON bridge_message_retry_queue(next_retry_at);
    )");
    const auto target_platform =
        mapping_source_platform == "qq" ? "telegram" : "qq";
    connection.execute(
        "INSERT INTO bridge_message_mappings(source_platform, "
        "source_message_id, target_platform, target_message_id, created_at) "
        "VALUES (?, 'legacy-message', ?, 'legacy-target', 1);",
        {mapping_source_platform, target_platform});
    execute_script(connection, R"(
      INSERT INTO bridge_message_retry_queue
        (source_platform, target_platform, source_message_id, message_content,
         group_id, source_group_id, target_topic_id, retry_count,
         max_retry_count, failure_reason, retry_type, next_retry_at,
         created_at, last_attempt_at)
      VALUES ('qq', 'telegram', 'legacy-retry', '[]', 'tg-group', 'qq-group',
              -1, 0, 5, 'offline', 'message_send', 1, 1, 1);
      INSERT INTO bridge_media_group_mappings
        (source_platform, media_group_id, source_message_id, target_platform,
         target_message_id, target_group_id, created_at)
      VALUES ('telegram', 'legacy-album', 'legacy-photo', 'qq',
              'legacy-combined', 'qq-group', 1);
      INSERT INTO bridge_users
        (platform, user_id, group_id, username, nickname, title, first_name,
         last_name, last_updated)
      VALUES ('qq', 'legacy-user', 'qq-group', '', 'Legacy', '', '', '', 1);
      INSERT INTO bridge_sticker_cache
        (platform, sticker_id, sticker_hash, original_name, file_type,
         mime_type, original_file_path, converted_file_path, container_path,
         file_size, conversion_status, created_at, last_used_at)
      VALUES ('telegram', 'legacy-sticker', 'legacy-hash', NULL, 'static',
              'image/webp', '/tmp/legacy.webp', NULL,
              '/container/legacy.webp', 10, 'none', 1, 1);
      INSERT INTO bridge_qq_sticker_mappings
        (qq_sticker_hash, telegram_file_id, file_type, created_at,
         last_used_at, is_gif, content_type, last_checked_at)
      VALUES ('legacy-qq-hash', 'legacy-tg-file', 'static', 1, 1, 0,
              'image/webp', 1);
      INSERT INTO bridge_platform_heartbeats
        (platform, last_heartbeat_at, updated_at) VALUES ('telegram', 1, 1);
    )");
    create_message_store_tables(connection);
    if (mapping_source_platform == "qq") {
      insert_stored_message(connection, "qq", std::string{kQqA},
                            "group:qq-group", "legacy-message");
    } else if (mapping_source_platform == "telegram") {
      insert_stored_message(connection, "telegram", std::string{kTgA},
                            "chat:tg-group", "legacy-message");
    }
    insert_stored_message(
        connection, "telegram", std::string{kTgA}, "chat:tg-group",
        "legacy-photo",
        R"({"media_group_id":"legacy-album","caption":"primary"})");
  });
}

} // namespace

TEST(BridgeDatabaseSchemaTest, CreatesVersionThreeAndUsesMigrationNamespace) {
  const auto path = temp_db_path("v3");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();

  EXPECT_EQ(repository.schema_version(), 3);
  EXPECT_TRUE(table_exists(*manager, "bridge_message_mappings"));
  EXPECT_TRUE(index_exists(*manager, "idx_bridge_message_mapping_reverse"));
  EXPECT_TRUE(index_exists(*manager, "idx_bridge_message_retry_next_retry"));
  EXPECT_TRUE(index_exists(*manager, "idx_bridge_media_group_lookup"));
  const auto lock_count = manager->run_read<std::int64_t>(
      "main", [](obcx::core::IDbConnection &connection) {
        return std::get<std::int64_t>(
            connection
                .query("SELECT COUNT(*) AS count FROM obcx_migration_locks "
                       "WHERE namespace = 'bridge';")
                .front()
                .at("count"));
      });
  EXPECT_EQ(lock_count, 1);
  repository.initialize_schema();
  EXPECT_EQ(repository.schema_version(), 3);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest,
     ConversationScopedMappingsPreserveCountsAndCrossChatCollisions) {
  const auto path = temp_db_path("mapping-scope");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();

  const auto first =
      mapping(std::string{kQqA}, "qq", "group:1012634788", "1162814500",
              std::string{kTgA}, "telegram", "chat:-1003150190260", "2700");
  const auto second =
      mapping(std::string{kQqA}, "qq", "group:823971580", "-1583402916",
              std::string{kTgA}, "telegram", "chat:-5281838703", "2700");
  repository.reset_message_mapping_operation_counts();
  ASSERT_TRUE(repository.add_message_mapping(
      first, bridge::MessageMappingWritePurpose::DirectForward));
  ASSERT_TRUE(repository.add_message_mapping(
      second, bridge::MessageMappingWritePurpose::DeferredMediaGroup));

  EXPECT_EQ(target_id(repository,
                      {.installation_id = std::string{kQqA},
                       .platform = "qq",
                       .conversation_id = "group:1012634788",
                       .message_id = "1162814500"},
                      {.installation_id = std::string{kTgA},
                       .platform = "telegram",
                       .conversation_id = "chat:-1003150190260"},
                      bridge::MessageMappingReadPurpose::PreSendDeduplication),
            "2700");
  const auto reverse = repository.resolve_source_mapping(
      {.installation_id = std::string{kTgA},
       .platform = "telegram",
       .conversation_id = "chat:-1003150190260",
       .message_id = "2700"},
      {.installation_id = std::string{kQqA},
       .platform = "qq",
       .conversation_id = "group:1012634788"});
  ASSERT_TRUE(reverse.unique());
  EXPECT_EQ(reverse.mapping->source_message_id, "1162814500");
  EXPECT_TRUE(
      repository
          .resolve_source_mapping({.installation_id = std::string{kTgA},
                                   .platform = "telegram",
                                   .conversation_id = "chat:-1003150190260",
                                   .message_id = "2700"},
                                  {.installation_id = std::string{kQqA},
                                   .platform = "qq",
                                   .conversation_id = "group:823971580"})
          .missing());

  const auto counts = repository.message_mapping_operation_counts();
  EXPECT_EQ(counts.direct_forward_writes, 1U);
  EXPECT_EQ(counts.deferred_media_group_writes, 1U);
  EXPECT_EQ(counts.pre_send_deduplication_reads, 1U);
  EXPECT_EQ(counts.general_reads, 2U);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, ScopesCachesRetriesMediaAndHeartbeats) {
  const auto path = temp_db_path("all-state");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();
  const auto now = std::chrono::system_clock::now();

  for (const auto installation : {kQqA, kQqB}) {
    storage::UserInfo user{.installation_id = std::string{installation},
                           .platform = "qq",
                           .user_id = "same-user",
                           .group_id = "same-group",
                           .nickname = std::string{installation},
                           .last_updated = now};
    ASSERT_TRUE(repository.save_or_update_user(user));
  }
  EXPECT_EQ(repository.query_user_display_name(std::string{kQqA}, "qq",
                                               "same-user", "same-group"),
            kQqA);

  storage::StickerCacheInfo sticker{.installation_id = std::string{kTgA},
                                    .platform = "telegram",
                                    .sticker_id = "same-sticker",
                                    .sticker_hash = "same-hash",
                                    .file_type = "static",
                                    .original_file_path = "/tmp/a",
                                    .container_path = "/container/a",
                                    .conversion_status = "none",
                                    .created_at = now,
                                    .last_used_at = now};
  ASSERT_TRUE(repository.save_sticker_cache(sticker));
  sticker.installation_id = std::string{kTgB};
  sticker.original_file_path = "/tmp/b";
  ASSERT_TRUE(repository.save_sticker_cache(sticker));
  EXPECT_EQ(
      repository.get_sticker_cache(std::string{kTgB}, "telegram", "same-hash")
          ->original_file_path,
      "/tmp/b");

  storage::QQStickerMapping qq_sticker{.source_installation = std::string{kQqA},
                                       .target_installation = std::string{kTgA},
                                       .qq_sticker_hash = "same-hash",
                                       .telegram_file_id = "file-a",
                                       .file_type = "static",
                                       .created_at = now,
                                       .last_used_at = now};
  ASSERT_TRUE(repository.save_qq_sticker_mapping(qq_sticker));

  storage::MessageRetryInfo retry{.source_installation = std::string{kQqA},
                                  .source_platform = "qq",
                                  .source_conversation_id = "group:qq-a",
                                  .target_installation = std::string{kTgA},
                                  .target_platform = "telegram",
                                  .target_conversation_id = "chat:tg-a",
                                  .source_message_id = "same-message",
                                  .message_content = "[]",
                                  .group_id = "tg-a",
                                  .source_group_id = "qq-a",
                                  .target_topic_id = -1,
                                  .retry_count = 0,
                                  .max_retry_count = 5,
                                  .failure_reason = "offline",
                                  .retry_type = "message_send",
                                  .next_retry_at = now,
                                  .created_at = now,
                                  .last_attempt_at = now};
  ASSERT_TRUE(repository.add_message_retry(retry));
  retry.source_conversation_id = "group:qq-b";
  retry.target_conversation_id = "chat:tg-b";
  retry.group_id = "tg-b";
  retry.source_group_id = "qq-b";
  ASSERT_TRUE(repository.add_message_retry(retry));
  EXPECT_EQ(repository
                .get_pending_message_retries(
                    std::chrono::system_clock::time_point::max(), 10)
                .size(),
            2U);

  ASSERT_TRUE(repository.add_media_group_mapping(
      {.source_installation = std::string{kTgA},
       .source_platform = "telegram",
       .source_conversation_id = "chat:tg-a",
       .media_group_id = "same-album",
       .source_message_id = "same-message",
       .target_installation = std::string{kQqA},
       .target_platform = "qq",
       .target_conversation_id = "group:qq-a",
       .target_message_id = "combined-a",
       .target_group_id = "qq-a",
       .is_primary = true,
       .created_at = now}));
  EXPECT_EQ(repository
                .get_media_group_mappings({.installation_id = std::string{kTgA},
                                           .platform = "telegram",
                                           .conversation_id = "chat:tg-a"},
                                          "same-album",
                                          {.installation_id = std::string{kQqA},
                                           .platform = "qq",
                                           .conversation_id = "group:qq-a"})
                .front()
                .target_message_id,
            "combined-a");

  ASSERT_TRUE(
      repository.update_platform_heartbeat(std::string{kTgA}, "telegram", now));
  EXPECT_EQ(
      repository.get_platform_heartbeat(std::string{kTgA})->installation_id,
      kTgA);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, ExactUpdateDeleteAndPrimaryValidation) {
  const auto path = temp_db_path("exact-mutations");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();
  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:a", "same", "tg-a", "telegram", "chat:a",
              "target", false)));
  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:b", "same", "tg-a", "telegram", "chat:b",
              "target", true)));
  ASSERT_TRUE(repository.update_message_mapping({.installation_id = "qq-a",
                                                 .platform = "qq",
                                                 .conversation_id = "group:a",
                                                 .message_id = "same"},
                                                {.installation_id = "tg-a",
                                                 .platform = "telegram",
                                                 .conversation_id = "chat:a"},
                                                "updated"));
  EXPECT_EQ(target_id(repository,
                      {.installation_id = "qq-a",
                       .platform = "qq",
                       .conversation_id = "group:a",
                       .message_id = "same"},
                      {.installation_id = "tg-a",
                       .platform = "telegram",
                       .conversation_id = "chat:a"}),
            "updated");
  EXPECT_EQ(target_id(repository,
                      {.installation_id = "qq-a",
                       .platform = "qq",
                       .conversation_id = "group:b",
                       .message_id = "same"},
                      {.installation_id = "tg-a",
                       .platform = "telegram",
                       .conversation_id = "chat:b"}),
            "target");
  ASSERT_TRUE(repository.delete_message_mapping({.installation_id = "qq-a",
                                                 .platform = "qq",
                                                 .conversation_id = "group:a",
                                                 .message_id = "same"},
                                                {.installation_id = "tg-a",
                                                 .platform = "telegram",
                                                 .conversation_id = "chat:a"}));
  EXPECT_TRUE(repository
                  .resolve_target_mapping({.installation_id = "qq-a",
                                           .platform = "qq",
                                           .conversation_id = "group:a",
                                           .message_id = "same"},
                                          {.installation_id = "tg-a",
                                           .platform = "telegram",
                                           .conversation_id = "chat:a"})
                  .missing());

  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:c", "one", "tg-a", "telegram", "chat:c",
              "fan-in", false)));
  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:c", "two", "tg-a", "telegram", "chat:c",
              "fan-in", false)));
  EXPECT_EQ(repository
                .resolve_source_mapping({.installation_id = "tg-a",
                                         .platform = "telegram",
                                         .conversation_id = "chat:c",
                                         .message_id = "fan-in"},
                                        {.installation_id = "qq-a",
                                         .platform = "qq",
                                         .conversation_id = "group:c"})
                .status,
            bridge::MappingResolutionStatus::Corrupt);
  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:d", "one", "tg-a", "telegram", "chat:d",
              "ambiguous", true)));
  ASSERT_TRUE(repository.add_message_mapping(
      mapping("qq-a", "qq", "group:d", "two", "tg-a", "telegram", "chat:d",
              "ambiguous", true)));
  EXPECT_EQ(repository
                .resolve_source_mapping({.installation_id = "tg-a",
                                         .platform = "telegram",
                                         .conversation_id = "chat:d",
                                         .message_id = "ambiguous"},
                                        {.installation_id = "qq-a",
                                         .platform = "qq",
                                         .conversation_id = "group:d"})
                .status,
            bridge::MappingResolutionStatus::Ambiguous);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, RejectsCorruptMediaPrimaryOnReopen) {
  const auto path = temp_db_path("media-primary-corrupt");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();
  const auto now = std::chrono::system_clock::now();
  for (const auto source_id : {"one", "two"}) {
    ASSERT_TRUE(repository.add_media_group_mapping(
        {.source_installation = "tg-a",
         .source_platform = "telegram",
         .source_conversation_id = "chat:tg-a",
         .media_group_id = "album",
         .source_message_id = source_id,
         .target_installation = "qq-a",
         .target_platform = "qq",
         .target_conversation_id = "group:qq-a",
         .target_message_id = "combined",
         .target_group_id = "qq-a",
         .is_primary = false,
         .created_at = now}));
  }
  bridge::BridgeStateRepository reopened(*manager, "main", "bridge");
  EXPECT_THROW(reopened.initialize_schema(), std::runtime_error);
  EXPECT_EQ(reopened.schema_version(), 3);
  EXPECT_EQ(table_count(*manager, "bridge_media_group_mappings"), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, RejectsIncompleteOrWrongConversationIdentity) {
  const auto path = temp_db_path("invalid-identity");
  auto manager = manager_for(path);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema();
  auto invalid =
      mapping("qq-a", "qq", "", "one", "tg-a", "telegram", "chat:a", "target");
  EXPECT_THROW(repository.add_message_mapping(invalid), std::invalid_argument);
  invalid.source_conversation_id = "chat:wrong";
  EXPECT_THROW(repository.add_message_mapping(invalid), std::invalid_argument);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 0);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MigratesVersionOneThroughVersionThree) {
  const auto path = temp_db_path("migrate-v1");
  auto manager = manager_for(path);
  create_complete_v1_fixture(*manager);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(migration_context());

  EXPECT_EQ(repository.schema_version(), 3);
  EXPECT_EQ(target_id(repository,
                      {.installation_id = std::string{kQqA},
                       .platform = "qq",
                       .conversation_id = "group:qq-group",
                       .message_id = "legacy-message"},
                      {.installation_id = std::string{kTgA},
                       .platform = "telegram",
                       .conversation_id = "chat:tg-group"}),
            "legacy-target");
  EXPECT_EQ(repository
                .get_pending_message_retries(
                    std::chrono::system_clock::time_point::max(), 10)
                .front()
                .source_conversation_id,
            "group:qq-group");
  EXPECT_TRUE(
      repository
          .get_media_group_mappings({.installation_id = std::string{kTgA},
                                     .platform = "telegram",
                                     .conversation_id = "chat:tg-group"},
                                    "legacy-album",
                                    {.installation_id = std::string{kQqA},
                                     .platform = "qq",
                                     .conversation_id = "group:qq-group"})
          .front()
          .is_primary);
  EXPECT_TRUE(
      repository.get_user(std::string{kQqA}, "qq", "legacy-user", "qq-group")
          .has_value());
  EXPECT_TRUE(
      repository.get_sticker_cache(std::string{kTgA}, "telegram", "legacy-hash")
          .has_value());
  EXPECT_TRUE(repository
                  .get_qq_sticker_mapping(std::string{kQqA}, std::string{kTgA},
                                          "legacy-qq-hash")
                  .has_value());
  EXPECT_TRUE(repository.get_platform_heartbeat(std::string{kTgA}).has_value());
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MigratesObserved2700CollisionByConversation) {
  const auto path = temp_db_path("collision-2700");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('qq-a','qq','1162814500','tg-a','telegram','2700',1),
             ('qq-a','qq','-1583402916','tg-a','telegram','2700',1);
    )");
    insert_stored_message(connection, "qq", "qq-a", "group:1012634788",
                          "1162814500");
    insert_stored_message(connection, "qq", "qq-a", "group:823971580",
                          "-1583402916");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(
      migration_context({route("chat:-1003150190260", "group:1012634788"),
                         route("chat:-5281838703", "group:823971580")}));

  const auto current = repository.resolve_source_mapping(
      {.installation_id = "tg-a",
       .platform = "telegram",
       .conversation_id = "chat:-1003150190260",
       .message_id = "2700"},
      {.installation_id = "qq-a",
       .platform = "qq",
       .conversation_id = "group:1012634788"});
  ASSERT_TRUE(current.unique());
  EXPECT_EQ(current.mapping->source_message_id, "1162814500");
  const auto historical =
      repository.resolve_source_mapping({.installation_id = "tg-a",
                                         .platform = "telegram",
                                         .conversation_id = "chat:-5281838703",
                                         .message_id = "2700"},
                                        {.installation_id = "qq-a",
                                         .platform = "qq",
                                         .conversation_id = "group:823971580"});
  ASSERT_TRUE(historical.unique());
  EXPECT_EQ(historical.mapping->source_message_id, "-1583402916");
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MigratesTelegramTopicFromStoredThreadEvidence) {
  const auto path = temp_db_path("topic-evidence");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('tg-a','telegram','topic-message','qq-a','qq','target',1);
    )");
    insert_stored_message(connection, "telegram", "tg-a", "chat:topic-chat",
                          "topic-message", R"({"message_thread_id":42})");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(migration_context(
      {route("chat:topic-chat", "group:wrong", "tg-a", "qq-a", 41),
       route("chat:topic-chat", "group:right", "tg-a", "qq-a", 42)}));
  EXPECT_EQ(target_id(repository,
                      {.installation_id = "tg-a",
                       .platform = "telegram",
                       .conversation_id = "chat:topic-chat",
                       .message_id = "topic-message"},
                      {.installation_id = "qq-a",
                       .platform = "qq",
                       .conversation_id = "group:right"}),
            "target");
  EXPECT_TRUE(repository
                  .resolve_target_mapping({.installation_id = "tg-a",
                                           .platform = "telegram",
                                           .conversation_id = "chat:topic-chat",
                                           .message_id = "topic-message"},
                                          {.installation_id = "qq-a",
                                           .platform = "qq",
                                           .conversation_id = "group:wrong"})
                  .missing());
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest,
     GroupToGroupMigrationAcceptsTelegramForumThreadMetadata) {
  const auto path = temp_db_path("group-route-with-thread-evidence");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    for (const auto &[source_id, target_id, topic_id] :
         std::vector<std::tuple<std::string, std::string, std::int64_t>>{
             {"2701", "-1007907160", 2700},
             {"2702", "-1254253969", 2700},
             {"2705", "894588964", 2703}}) {
      insert_stored_message(
          connection, "telegram", "tg-a", "chat:-1003150190260", source_id,
          "{\"message_thread_id\":" + std::to_string(topic_id) + "}");
      connection.execute(R"(
        INSERT INTO bridge_message_mappings
          (source_installation, source_platform, source_message_id,
           target_installation, target_platform, target_message_id, created_at)
        VALUES ('tg-a','telegram',?,'qq-a','qq',?,1);
      )",
                         {source_id, target_id});
    }
  });

  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(
      migration_context({route("chat:-1003150190260", "group:1012634788")}));

  EXPECT_EQ(repository.schema_version(), 3);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 3);
  for (const auto &[source_id, target_message_id] :
       std::vector<std::pair<std::string, std::string>>{
           {"2701", "-1007907160"},
           {"2702", "-1254253969"},
           {"2705", "894588964"}}) {
    EXPECT_EQ(target_id(repository,
                        {.installation_id = "tg-a",
                         .platform = "telegram",
                         .conversation_id = "chat:-1003150190260",
                         .message_id = source_id},
                        {.installation_id = "qq-a",
                         .platform = "qq",
                         .conversation_id = "group:1012634788"}),
              target_message_id);
  }
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MissingThreadEvidenceDoesNotGuessTopicRoute) {
  const auto path = temp_db_path("missing-topic-evidence");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('tg-a','telegram','no-topic','qq-a','qq','target',1);
    )");
    insert_stored_message(connection, "telegram", "tg-a", "chat:topic-chat",
                          "no-topic");
  });

  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(
      repository.initialize_schema(migration_context({route(
          "chat:topic-chat", "group:topic-target", "tg-a", "qq-a", 42)})),
      std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MigratesAlbumWithOneSemanticPrimary) {
  const auto path = temp_db_path("album-primary");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    for (const auto &[message_id, payload] :
         std::vector<std::pair<std::string, std::string>>{
             {"photo-1", R"({"media_group_id":"album","caption":"primary"})"},
             {"photo-2", R"({"media_group_id":"album"})"}}) {
      insert_stored_message(connection, "telegram", "tg-a", "chat:tg-group",
                            message_id, payload);
      connection.execute(R"(
        INSERT INTO bridge_message_mappings
          (source_installation, source_platform, source_message_id,
           target_installation, target_platform, target_message_id, created_at)
        VALUES ('tg-a','telegram',?,'qq-a','qq','combined',1);
      )",
                         {message_id});
      connection.execute(R"(
        INSERT INTO bridge_media_group_mappings
          (source_installation, source_platform, media_group_id,
           source_message_id, target_installation, target_platform,
           target_message_id, target_group_id, created_at)
        VALUES ('tg-a','telegram','album',?,'qq-a','qq','combined',
                'qq-group',1);
      )",
                         {message_id});
    }
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(migration_context());

  const auto reverse =
      repository.resolve_source_mapping({.installation_id = "qq-a",
                                         .platform = "qq",
                                         .conversation_id = "group:qq-group",
                                         .message_id = "combined"},
                                        {.installation_id = "tg-a",
                                         .platform = "telegram",
                                         .conversation_id = "chat:tg-group"});
  ASSERT_TRUE(reverse.unique());
  EXPECT_EQ(reverse.mapping->source_message_id, "photo-1");
  const auto media = repository.get_media_group_mappings(
      {.installation_id = "tg-a",
       .platform = "telegram",
       .conversation_id = "chat:tg-group"},
      "album",
      {.installation_id = "qq-a",
       .platform = "qq",
       .conversation_id = "group:qq-group"});
  ASSERT_EQ(media.size(), 2U);
  EXPECT_EQ(std::ranges::count_if(
                media, [](const auto &row) { return row.is_primary; }),
            1);
  EXPECT_EQ(media.front().source_message_id, "photo-1");
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, AlbumWithoutPrimaryEvidenceFailsClosed) {
  const auto path = temp_db_path("album-no-primary");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    for (const auto message_id : {"photo-1", "photo-2"}) {
      insert_stored_message(connection, "telegram", "tg-a", "chat:tg-group",
                            message_id, R"({"media_group_id":"album"})");
      connection.execute(R"(
        INSERT INTO bridge_message_mappings
          (source_installation, source_platform, source_message_id,
           target_installation, target_platform, target_message_id, created_at)
        VALUES ('tg-a','telegram',?,'qq-a','qq','combined',1);
      )",
                         {message_id});
      connection.execute(R"(
        INSERT INTO bridge_media_group_mappings
          (source_installation, source_platform, media_group_id,
           source_message_id, target_installation, target_platform,
           target_message_id, target_group_id, created_at)
        VALUES ('tg-a','telegram','album',?,'qq-a','qq','combined',
                'qq-group',1);
      )",
                         {message_id});
    }
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context()),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MultiConversationSourceHistoryIsAmbiguous) {
  const auto path = temp_db_path("source-ambiguous");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('qq-a','qq','same','tg-a','telegram','target',1);
    )");
    insert_stored_message(connection, "qq", "qq-a", "group:a", "same");
    insert_stored_message(connection, "qq", "qq-a", "group:b", "same");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context(
                   {route("chat:a", "group:a"), route("chat:b", "group:b")})),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest,
     StrictVersionTwoMigrationRollsBackUnresolvedRows) {
  const auto path = temp_db_path("strict-rollback");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('qq-a','qq','missing','tg-a','telegram','1',1);
    )");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context()),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 1);
  EXPECT_FALSE(table_exists(*manager, "bridge_message_mappings_obcx_v2"));
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, ExplicitArchiveIsNonLiveAndCountPreserving) {
  const auto path = temp_db_path("archive");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      VALUES ('qq-a','qq','missing','tg-a','telegram','1',1);
    )");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(
      migration_context({route()}, LegacyUnresolvedMappingPolicy::Archive));
  EXPECT_EQ(repository.schema_version(), 3);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), 0);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings_v2_archive"), 1);
  EXPECT_TRUE(repository
                  .resolve_target_mapping({.installation_id = "qq-a",
                                           .platform = "qq",
                                           .conversation_id = "group:qq-group",
                                           .message_id = "missing"},
                                          {.installation_id = "tg-a",
                                           .platform = "telegram",
                                           .conversation_id = "chat:tg-group"})
                  .missing());
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, ArchivePolicyCannotHideUnresolvedRetry) {
  const auto path = temp_db_path("retry-blocks");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    connection.execute(R"(
      INSERT INTO bridge_message_retry_queue
        (source_installation, source_platform, target_installation,
         target_platform, source_message_id, message_content, group_id,
         source_group_id, target_topic_id, retry_count, max_retry_count,
         failure_reason, retry_type, next_retry_at, created_at, last_attempt_at)
      VALUES ('qq-a','qq','tg-a','telegram','retry','[]','tg-group',NULL,-1,
              0,5,'offline','message_send',1,1,1);
    )");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context(
                   {route()}, LegacyUnresolvedMappingPolicy::Archive)),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  EXPECT_EQ(table_count(*manager, "bridge_message_retry_queue"), 1);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, MigrationRequiresProcessRestart) {
  const auto path = temp_db_path("restart");
  auto manager = manager_for(path);
  manager->run_write<void>("main", create_v2_tables);
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context(
                   {route()}, LegacyUnresolvedMappingPolicy::Fail, false)),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, InvalidVersionOneRowsRollBackAtomically) {
  const auto path = temp_db_path("v1-rollback");
  auto manager = manager_for(path);
  create_complete_v1_fixture(*manager, "discord");
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context()),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 1);
  EXPECT_TRUE(table_exists(*manager, "bridge_message_mappings"));
  EXPECT_FALSE(table_exists(*manager, "bridge_message_mappings_obcx_v1"));
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, RejectsMalformedVersionTwoShapeWithoutMutation) {
  const auto path = temp_db_path("malformed-v2");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    execute_script(connection, R"(
      CREATE TABLE bridge_schema_version(version INTEGER NOT NULL);
      INSERT INTO bridge_schema_version VALUES (2);
      CREATE TABLE bridge_message_mappings(id INTEGER PRIMARY KEY);
    )");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(migration_context()),
               std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 2);
  EXPECT_TRUE(table_exists(*manager, "bridge_message_mappings"));
  EXPECT_FALSE(table_exists(*manager, "bridge_message_mappings_obcx_v2"));
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, RejectsNewerSchemaWithoutMutation) {
  const auto path = temp_db_path("newer");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute(
        "CREATE TABLE bridge_schema_version(version INTEGER NOT NULL);");
    connection.execute("INSERT INTO bridge_schema_version VALUES (99);");
  });
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  EXPECT_THROW(repository.initialize_schema(), std::runtime_error);
  EXPECT_EQ(repository.schema_version(), 99);
  EXPECT_FALSE(table_exists(*manager, "bridge_message_mappings"));
  std::filesystem::remove(path);
}

TEST(BridgeDatabaseSchemaTest, ProductionScaleMigrationPreservesSchemaAndRows) {
  constexpr std::int64_t row_count = 102421;
  const auto path = temp_db_path("production-scale");
  auto manager = manager_for(path);
  manager->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    create_v2_tables(connection);
    create_message_store_tables(connection);
    execute_script(connection, R"(
      CREATE TEMP TABLE bridge_scale_numbers(n INTEGER PRIMARY KEY);
      WITH digits(d) AS (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9))
      INSERT INTO bridge_scale_numbers
      SELECT a.d + 10*b.d + 100*c.d + 1000*d.d + 10000*e.d + 100000*f.d
      FROM digits a CROSS JOIN digits b CROSS JOIN digits c
      CROSS JOIN digits d CROSS JOIN digits e CROSS JOIN digits f
      WHERE a.d + 10*b.d + 100*c.d + 1000*d.d + 10000*e.d + 100000*f.d
            < 102421;
      INSERT INTO message_store_qq_messages
        (message_id, source_platform, source_bot, conversation_id, payload,
         raw, timestamp, created_at, updated_at)
      SELECT CAST(n AS TEXT), 'qq', 'qq-a', 'group:qq-group', '{}', '{}',
             1, 1, 1 FROM bridge_scale_numbers;
      INSERT INTO bridge_message_mappings
        (source_installation, source_platform, source_message_id,
         target_installation, target_platform, target_message_id, created_at)
      SELECT 'qq-a', 'qq', CAST(n AS TEXT), 'tg-a', 'telegram',
             CAST(n AS TEXT), 1 FROM bridge_scale_numbers;
      DROP TABLE bridge_scale_numbers;
    )");
  });
  const auto schema_before = manager->run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        return std::get<std::string>(
            connection
                .query("SELECT sql FROM sqlite_master WHERE "
                       "name='message_store_qq_messages';")
                .front()
                .at("sql"));
      });
  const auto started = std::chrono::steady_clock::now();
  bridge::BridgeStateRepository repository(*manager, "main", "bridge");
  repository.initialize_schema(migration_context());
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(repository.schema_version(), 3);
  EXPECT_EQ(table_count(*manager, "bridge_message_mappings"), row_count);
  EXPECT_LT(elapsed, std::chrono::seconds{60});
  const auto schema_after = manager->run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        return std::get<std::string>(
            connection
                .query("SELECT sql FROM sqlite_master WHERE "
                       "name='message_store_qq_messages';")
                .front()
                .at("sql"));
      });
  EXPECT_EQ(schema_after, schema_before);
  std::filesystem::remove(path);
}

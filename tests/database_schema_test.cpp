#include "bridge_state_repository.hpp"
#include "bridge_storage_models.hpp"
#include "common/config_loader.hpp"
#include "core/db_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

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

auto table_exists(const std::filesystem::path &db_path,
                  const std::string &table_name) -> bool {
  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config("inspect", db_path)});
  return db_manager.run_read<bool>(
      "inspect", [&](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(R"(
	      SELECT 1 FROM sqlite_master
	      WHERE type = 'table' AND name = ?
	      LIMIT 1;
	  )",
                                           {table_name});
        return !rows.empty();
      });
}

} // namespace

TEST(BridgeDatabaseSchemaTest,
     BridgeStateRepositoryUsesCoreDbManagerNamespace) {
  const auto db_path = temp_db_path("db_manager_namespace");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config("main", db_path)});

  bridge::BridgeStateRepository repository(db_manager, "main", "bridge");
  repository.initialize_schema();

  const storage::MessageMapping mapping{
      .source_platform = "telegram",
      .source_message_id = "tg-1",
      .target_platform = "qq",
      .target_message_id = "qq-8",
      .created_at = std::chrono::system_clock::now(),
  };

  repository.add_message_mapping(mapping);

  EXPECT_TRUE(table_exists(db_path, "bridge_message_mappings"));
  EXPECT_FALSE(table_exists(db_path, "message_mappings"));
  EXPECT_EQ(repository.get_target_message_id("telegram", "tg-1", "qq"), "qq-8");

  const auto lock_count = db_manager.run_read<std::int64_t>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query(
            "SELECT COUNT(*) AS count FROM obcx_migration_locks "
            "WHERE namespace = ?;",
            {std::string{"bridge"}});
        return std::get<std::int64_t>(rows.at(0).at("count"));
      });
  EXPECT_EQ(lock_count, 1);

  std::filesystem::remove(db_path);
}

TEST(BridgeDatabaseSchemaTest, BridgeStateRepositoryPersistsBridgeOwnedCacheState) {
  const auto db_path = temp_db_path("cache_state");
  const auto created_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{10}};
  const auto used_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{20}};
  const auto checked_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{30}};
  const auto heartbeat_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{40}};

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config("main", db_path)});

  bridge::BridgeStateRepository repository(db_manager, "main", "bridge");
  repository.initialize_schema();

  storage::UserInfo telegram_user;
  telegram_user.platform = "telegram";
  telegram_user.user_id = "tg-user";
  telegram_user.first_name = "Ada";
  telegram_user.last_name = "Lovelace";
  telegram_user.last_updated = std::chrono::system_clock::now();
  ASSERT_TRUE(repository.save_or_update_user(telegram_user));
  EXPECT_EQ(repository.query_user_display_name("telegram", "tg-user"), "Ada Lovelace");

  storage::StickerCacheInfo sticker;
  sticker.platform = "telegram";
  sticker.sticker_id = "sticker-1";
  sticker.sticker_hash = "hash-1";
  sticker.original_name = "wave.webp";
  sticker.file_type = "static";
  sticker.mime_type = "image/webp";
  sticker.original_file_path = "/tmp/wave.webp";
  sticker.converted_file_path = std::nullopt;
  sticker.container_path = "/container/wave.webp";
  sticker.file_size = 128;
  sticker.conversion_status = "none";
  sticker.created_at = created_at;
  sticker.last_used_at = used_at;
  ASSERT_TRUE(repository.save_sticker_cache(sticker));

  const auto stored_sticker =
      repository.get_sticker_cache("telegram", "hash-1");
  ASSERT_TRUE(stored_sticker.has_value());
  EXPECT_EQ(stored_sticker->original_name, "wave.webp");
  EXPECT_EQ(stored_sticker->file_size, 128);

  ASSERT_TRUE(repository.update_sticker_conversion("telegram", "hash-1",
                                                   "success",
                                                   "/tmp/wave.png"));
  const auto converted_sticker =
      repository.get_sticker_cache("telegram", "hash-1");
  ASSERT_TRUE(converted_sticker.has_value());
  EXPECT_EQ(converted_sticker->conversion_status, "success");
  EXPECT_EQ(converted_sticker->converted_file_path, "/tmp/wave.png");

  storage::QQStickerMapping mapping;
  mapping.qq_sticker_hash = "qq-hash-1";
  mapping.telegram_file_id = "tg-file-1";
  mapping.file_type = "animation";
  mapping.created_at = created_at;
  mapping.last_used_at = used_at;
  mapping.is_gif = true;
  mapping.content_type = "image/gif";
  mapping.last_checked_at = checked_at;
  ASSERT_TRUE(repository.save_qq_sticker_mapping(mapping));

  const auto stored_mapping =
      repository.get_qq_sticker_mapping("qq-hash-1");
  ASSERT_TRUE(stored_mapping.has_value());
  EXPECT_EQ(stored_mapping->telegram_file_id, "tg-file-1");
  EXPECT_EQ(stored_mapping->is_gif, true);
  EXPECT_EQ(stored_mapping->content_type, "image/gif");

  ASSERT_TRUE(repository.update_platform_heartbeat("qq", heartbeat_at));
  const auto heartbeat = repository.get_platform_heartbeat("qq");
  ASSERT_TRUE(heartbeat.has_value());
  EXPECT_EQ(heartbeat->platform, "qq");
  EXPECT_EQ(heartbeat->last_heartbeat_at, heartbeat_at);

  EXPECT_TRUE(table_exists(db_path, "bridge_users"));
  EXPECT_TRUE(table_exists(db_path, "bridge_sticker_cache"));
  EXPECT_TRUE(table_exists(db_path, "bridge_qq_sticker_mappings"));
  EXPECT_TRUE(table_exists(db_path, "bridge_platform_heartbeats"));
  EXPECT_FALSE(table_exists(db_path, "users"));
  EXPECT_FALSE(table_exists(db_path, "sticker_cache"));
  EXPECT_FALSE(table_exists(db_path, "qq_sticker_mapping"));
  EXPECT_FALSE(table_exists(db_path, "platform_heartbeats"));

  std::filesystem::remove(db_path);
}

TEST(BridgeDatabaseSchemaTest,
     BridgeStateRepositoryRestoresReplyRecallMappings) {
  const auto db_path = temp_db_path("reply_recall_mapping");

  obcx::core::DbManager first_db_manager;
  first_db_manager.configure({sqlite_config("main", db_path)});
  bridge::BridgeStateRepository first_repository(first_db_manager, "main",
                                                 "bridge");
  first_repository.initialize_schema();

  const storage::MessageMapping mapping{
      .source_platform = "qq",
      .source_message_id = "qq-recalled",
      .target_platform = "telegram",
      .target_message_id = "tg-forwarded",
      .created_at = std::chrono::system_clock::now(),
  };
  ASSERT_TRUE(first_repository.add_message_mapping(mapping));

  obcx::core::DbManager second_db_manager;
  second_db_manager.configure({sqlite_config("main", db_path)});
  bridge::BridgeStateRepository second_repository(second_db_manager, "main",
                                                  "bridge");

  EXPECT_EQ(
      second_repository.get_target_message_id("qq", "qq-recalled", "telegram"),
      "tg-forwarded");
  EXPECT_EQ(
      second_repository.get_source_message_id("telegram", "tg-forwarded", "qq"),
      "qq-recalled");

  std::filesystem::remove(db_path);
}

TEST(BridgeDatabaseSchemaTest, BridgeStateRepositoryUpdatesAndDeletesMappings) {
  const auto db_path = temp_db_path("mapping_update_delete");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config("main", db_path)});

  bridge::BridgeStateRepository repository(db_manager, "main", "bridge");
  repository.initialize_schema();

  const storage::MessageMapping mapping{
      .source_platform = "telegram",
      .source_message_id = "tg-edit",
      .target_platform = "qq",
      .target_message_id = "qq-original",
      .created_at = std::chrono::system_clock::now(),
  };
  ASSERT_TRUE(repository.add_message_mapping(mapping));

  ASSERT_TRUE(repository.update_message_mapping("telegram", "tg-edit", "qq",
                                                "qq-edited"));
  EXPECT_EQ(repository.get_target_message_id("telegram", "tg-edit", "qq"),
            "qq-edited");
  EXPECT_EQ(repository.get_source_message_id("qq", "qq-edited", "telegram"),
            "tg-edit");

  ASSERT_TRUE(repository.delete_message_mapping("telegram", "tg-edit", "qq"));
  EXPECT_FALSE(repository.get_target_message_id("telegram", "tg-edit", "qq")
                   .has_value());
  EXPECT_FALSE(repository.get_source_message_id("qq", "qq-edited", "telegram")
                   .has_value());

  std::filesystem::remove(db_path);
}

TEST(BridgeDatabaseSchemaTest, BridgeStateRepositoryPersistsRetryState) {
  const auto db_path = temp_db_path("retry_state");

  obcx::core::DbManager first_db_manager;
  first_db_manager.configure({sqlite_config("main", db_path)});

  bridge::BridgeStateRepository first_repository(first_db_manager, "main",
                                                 "bridge");
  first_repository.initialize_schema();

  storage::MessageRetryInfo retry;
  retry.source_platform = "qq";
  retry.target_platform = "telegram";
  retry.source_message_id = "qq-retry-1";
  retry.message_content = R"([{"type":"text","data":{"text":"hello"}}])";
  retry.group_id = "tg-group";
  retry.source_group_id = "qq-group";
  retry.target_topic_id = 42;
  retry.retry_count = 1;
  retry.max_retry_count = 5;
  retry.failure_reason = "timeout";
  retry.retry_type = "message_send";
  retry.next_retry_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{100}};
  retry.created_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{10}};
  retry.last_attempt_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{20}};

  ASSERT_TRUE(first_repository.add_message_retry(retry));

  obcx::core::DbManager second_db_manager;
  second_db_manager.configure({sqlite_config("main", db_path)});
  bridge::BridgeStateRepository second_repository(second_db_manager, "main",
                                                  "bridge");

  const auto pending = second_repository.get_pending_message_retries(
      std::chrono::system_clock::time_point{std::chrono::seconds{101}}, 10);
  ASSERT_EQ(pending.size(), 1);
  EXPECT_EQ(pending.front().source_platform, "qq");
  EXPECT_EQ(pending.front().target_platform, "telegram");
  EXPECT_EQ(pending.front().source_message_id, "qq-retry-1");
  EXPECT_EQ(pending.front().message_content, retry.message_content);
  EXPECT_EQ(pending.front().target_topic_id, 42);
  EXPECT_EQ(pending.front().retry_count, 1);
  EXPECT_EQ(pending.front().failure_reason, "timeout");

  ASSERT_TRUE(second_repository.update_message_retry(
      "qq", "qq-retry-1", "telegram", 2,
      std::chrono::system_clock::time_point{std::chrono::seconds{200}},
      "still-timeout"));
  const auto not_ready = second_repository.get_pending_message_retries(
      std::chrono::system_clock::time_point{std::chrono::seconds{199}}, 10);
  EXPECT_TRUE(not_ready.empty());

  ASSERT_TRUE(
      second_repository.remove_message_retry("qq", "qq-retry-1", "telegram"));
  const auto after_remove = second_repository.get_pending_message_retries(
      std::chrono::system_clock::time_point{std::chrono::seconds{300}}, 10);
  EXPECT_TRUE(after_remove.empty());

  EXPECT_TRUE(table_exists(db_path, "bridge_message_retry_queue"));
  EXPECT_FALSE(table_exists(db_path, "message_retry_queue"));

  std::filesystem::remove(db_path);
}

TEST(BridgeDatabaseSchemaTest,
     BridgeStateRepositoryPersistsMediaGroupMappings) {
  const auto db_path = temp_db_path("media_group_state");

  obcx::core::DbManager first_db_manager;
  first_db_manager.configure({sqlite_config("main", db_path)});

  bridge::BridgeStateRepository first_repository(first_db_manager, "main",
                                                 "bridge");
  first_repository.initialize_schema();

  const auto created_at =
      std::chrono::system_clock::time_point{std::chrono::seconds{50}};
  ASSERT_TRUE(first_repository.add_media_group_mapping(
      bridge::MediaGroupMapping{.source_platform = "telegram",
                                .media_group_id = "album-42",
                                .source_message_id = "tg-a",
                                .target_platform = "qq",
                                .target_message_id = "qq-combined",
                                .target_group_id = "qq-group",
                                .created_at = created_at}));
  ASSERT_TRUE(first_repository.add_media_group_mapping(
      bridge::MediaGroupMapping{.source_platform = "telegram",
                                .media_group_id = "album-42",
                                .source_message_id = "tg-b",
                                .target_platform = "qq",
                                .target_message_id = "qq-combined",
                                .target_group_id = "qq-group",
                                .created_at = created_at}));

  obcx::core::DbManager second_db_manager;
  second_db_manager.configure({sqlite_config("main", db_path)});
  bridge::BridgeStateRepository second_repository(second_db_manager, "main",
                                                  "bridge");

  const auto mappings =
      second_repository.get_media_group_mappings("telegram", "album-42", "qq");
  ASSERT_EQ(mappings.size(), 2);
  EXPECT_EQ(mappings.at(0).source_message_id, "tg-a");
  EXPECT_EQ(mappings.at(0).target_message_id, "qq-combined");
  EXPECT_EQ(mappings.at(1).source_message_id, "tg-b");
  EXPECT_EQ(mappings.at(1).target_message_id, "qq-combined");

  EXPECT_TRUE(table_exists(db_path, "bridge_media_group_mappings"));

  std::filesystem::remove(db_path);
}

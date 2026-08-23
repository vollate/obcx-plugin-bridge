#include "common/config_loader.hpp"
#include "core/db_manager.hpp"
#include "received_message_repository.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("obcx_received_repo_" + name + "_" + std::to_string(stamp) +
          ".sqlite3");
}

auto sqlite_config(const std::filesystem::path &path)
    -> obcx::common::DbInstanceConfig {
  obcx::common::DbInstanceConfig config;
  config.name = "main";
  config.type = "sqlite";
  config.path = path.string();
  return config;
}

} // namespace

TEST(ReceivedMessageRepositoryTest, ReadsMessageStoreOwnedRawMessages) {
  const auto db_path = temp_db_path("message_lookup");

  obcx::core::DbManager db_manager;
  db_manager.configure({sqlite_config(db_path)});
  db_manager.run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute(R"(
          CREATE TABLE message_store_qq_messages (
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
            UNIQUE(source_platform, source_bot, conversation_id, message_id)
          );
        )");
    connection.execute(
        R"(
          INSERT INTO message_store_qq_messages
            (message_id, source_platform, source_bot, conversation_id,
             sender, group_id, message_type, payload, raw, timestamp,
             created_at, updated_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )",
        {std::string{"qq-img-1"}, std::string{"qq"}, std::string{"qq-main"},
         std::string{"group:group-9"}, std::string{"user-7"},
         std::string{"group-9"}, std::string{"image"},
         std::string{R"({"text":"caption"})"},
         std::string{R"({"message":[{"type":"image"}]})"},
         static_cast<std::int64_t>(123000), static_cast<std::int64_t>(123000),
         static_cast<std::int64_t>(124000)});
  });

  bridge::ReceivedMessageRepository repository(db_manager, "main",
                                               "message_store");

  const auto message =
      repository.get_message("qq", "qq-main", "group:group-9", "qq-img-1");
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->platform, "qq");
  EXPECT_EQ(message->source_bot, "qq-main");
  EXPECT_EQ(message->conversation_id, "group:group-9");
  EXPECT_EQ(message->message_id, "qq-img-1");
  EXPECT_EQ(message->group_id, "group-9");
  EXPECT_EQ(message->user_id, "user-7");
  EXPECT_EQ(message->message_type, "image");
  EXPECT_EQ(message->content, "caption");
  EXPECT_EQ(message->raw_message, R"({"message":[{"type":"image"}]})");
  EXPECT_FALSE(
      repository.get_message("qq", "qq-secondary", "group:group-9", "qq-img-1")
          .has_value());
  EXPECT_FALSE(
      repository.get_message("qq", "qq-main", "group:other", "qq-img-1")
          .has_value());

  std::filesystem::remove(db_path);
}

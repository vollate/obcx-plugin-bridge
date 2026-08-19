#include "bridge_bot_operations.hpp"
#include "bridge_state_repository.hpp"
#include "common/config_loader.hpp"
#include "core/blocking_executor.hpp"
#include "core/db_manager.hpp"
#include "qq/command_handler.hpp"
#include "received_message_repository.hpp"
#include "telegram/command_handler.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace {

class RecordingOperationClient final : public obcx::bot::BotOperationClient {
public:
  auto supported_actions(const obcx::bot::BotInstallationRef &installation)
      const -> obcx::bot::BotOperationResult<
          obcx::bot::SupportedBotActions> override {
    return obcx::bot::BotOperationResult<
        obcx::bot::SupportedBotActions>::success({.installation =
                                                      installation});
  }

  auto
  execute(const obcx::bot::SendGroupMessageRequest &request) -> asio::awaitable<
      obcx::bot::BotOperationResult<obcx::bot::SendMessageResult>> override {
    sends.push_back(request);
    co_return obcx::bot::BotOperationResult<obcx::bot::SendMessageResult>::
        success({.messages = {{.group = request.target,
                               .native_message_id =
                                   "sent-" + std::to_string(sends.size())}}});
  }

  auto
  execute(const obcx::bot::DeleteMessageRequest &request) -> asio::awaitable<
      obcx::bot::BotOperationResult<obcx::bot::DeleteMessageResult>> override {
    deletions.push_back(request);
    co_return obcx::bot::BotOperationResult<
        obcx::bot::DeleteMessageResult>::success({.message = request.message});
  }

  auto execute(const obcx::bot::PokeOneBotGroupRequest &request)
      -> asio::awaitable<obcx::bot::BotOperationResult<
          obcx::bot::OneBotGroupPokeResult>> override {
    pokes.push_back(request);
    co_return obcx::bot::BotOperationResult<
        obcx::bot::OneBotGroupPokeResult>::success({.target = request.target,
                                                    .user_id =
                                                        request.user_id});
  }

  std::vector<obcx::bot::SendGroupMessageRequest> sends;
  std::vector<obcx::bot::DeleteMessageRequest> deletions;
  std::vector<obcx::bot::PokeOneBotGroupRequest> pokes;
};

template <typename T> auto run(asio::awaitable<T> operation) -> T {
  asio::io_context context;
  auto future = asio::co_spawn(context, std::move(operation), asio::use_future);
  context.run();
  return future.get();
}

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("obcx_bridge_commands_" + name + "_" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".sqlite3");
}

auto sqlite_config(const std::filesystem::path &path)
    -> obcx::common::DbInstanceConfig {
  return {.name = "main", .type = "sqlite", .path = path.string()};
}

auto text_from(const obcx::common::Message &message) -> std::string {
  std::string text;
  for (const auto &segment : message) {
    if (segment.type == "text") {
      text += segment.data.value("text", std::string{});
    }
  }
  return text;
}

TEST(BridgeCommandOperationTest, RecallUsesTypedDeleteAndReplySend) {
  const auto db_path = temp_db_path("recall");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping({
      .source_platform = "telegram",
      .source_message_id = "100",
      .target_platform = "qq",
      .target_message_id = "200",
      .created_at = std::chrono::system_clock::now(),
  }));
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::telegram::TelegramCommandHandler handler(operations, repository,
                                                   nullptr, blocking);
  obcx::common::MessageEvent event;
  event.group_id = "tg-group";
  event.message_id = "300";
  event.data = {{"reply_to_message", {{"message_id", 100}}}};

  run(handler.handle_recall_command(std::move(event), "qq-group"));

  ASSERT_EQ(client->deletions.size(), 1U);
  EXPECT_EQ(client->deletions[0].message.group.native_group_id, "qq-group");
  EXPECT_EQ(client->deletions[0].message.native_message_id, "200");
  ASSERT_EQ(client->sends.size(), 1U);
  EXPECT_EQ(client->sends[0].target.installation.surface,
            obcx::bot::BotSurface::TelegramBotApi);
  EXPECT_NE(text_from(client->sends[0].message).find("撤回成功"),
            std::string::npos);
  EXPECT_FALSE(
      repository->get_target_message_id("telegram", "100", "qq").has_value());

  blocking->shutdown();
  std::filesystem::remove(db_path);
}

TEST(BridgeCommandOperationTest, CheckaliveRepliesThroughTypedGroupSends) {
  const auto db_path = temp_db_path("checkalive");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  const auto now = std::chrono::system_clock::now();
  ASSERT_TRUE(repository->update_platform_heartbeat("qq", now));
  ASSERT_TRUE(repository->update_platform_heartbeat("telegram", now));
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);

  bridge::telegram::TelegramCommandHandler telegram_handler(
      operations, repository, nullptr, blocking);
  obcx::common::MessageEvent telegram_event;
  telegram_event.group_id = "tg-group";
  telegram_event.message_id = "tg-command";
  run(telegram_handler.handle_checkalive_command(std::move(telegram_event),
                                                 "qq-group"));

  bridge::qq::QQCommandHandler qq_handler(operations, repository, blocking);
  obcx::common::MessageEvent qq_event;
  qq_event.group_id = "qq-group";
  qq_event.message_id = "qq-command";
  run(qq_handler.handle_checkalive_command(std::move(qq_event), "tg-group"));

  ASSERT_EQ(client->sends.size(), 2U);
  EXPECT_EQ(client->sends[0].target.installation.surface,
            obcx::bot::BotSurface::TelegramBotApi);
  EXPECT_EQ(client->sends[1].target.installation.surface,
            obcx::bot::BotSurface::OneBot11Qq);
  EXPECT_NE(text_from(client->sends[0].message).find("QQ平台状态"),
            std::string::npos);

  blocking->shutdown();
  std::filesystem::remove(db_path);
}

TEST(BridgeCommandOperationTest, PokeUsesTypedOneBotAction) {
  const auto db_path = temp_db_path("poke");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  db->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute(R"(
      CREATE TABLE message_store_telegram_messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        message_id TEXT NOT NULL,
        source_platform TEXT NOT NULL,
        source_bot TEXT NOT NULL DEFAULT '',
        sender TEXT NOT NULL DEFAULT '',
        group_id TEXT NOT NULL DEFAULT '',
        message_type TEXT NOT NULL DEFAULT 'unknown',
        payload TEXT NOT NULL DEFAULT '{}',
        raw TEXT NOT NULL DEFAULT '{}',
        timestamp INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        UNIQUE(source_platform, source_bot, message_id)
      );
    )");
    connection.execute(
        R"(
          INSERT INTO message_store_telegram_messages
            (message_id, source_platform, source_bot, sender, group_id,
             message_type, payload, raw, timestamp, created_at, updated_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )",
        {std::string{"100"}, std::string{"telegram"}, std::string{"tg-main"},
         std::string{"qq-user"}, std::string{"tg-group"}, std::string{"group"},
         std::string{"{}"}, std::string{"{}"}, std::int64_t{1}, std::int64_t{1},
         std::int64_t{1}});
  });
  auto received = std::make_shared<bridge::ReceivedMessageRepository>(
      *db, "main", "message_store");
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::telegram::TelegramCommandHandler handler(operations, repository,
                                                   received, blocking);
  obcx::common::MessageEvent event;
  event.group_id = "tg-group";
  event.message_id = "command";
  event.data = {{"reply_to_message", {{"message_id", 100}}}};

  run(handler.handle_poke_command(std::move(event), "qq-group"));

  ASSERT_EQ(client->pokes.size(), 1U);
  EXPECT_EQ(client->pokes[0].target.native_group_id, "qq-group");
  EXPECT_EQ(client->pokes[0].user_id, "qq-user");

  blocking->shutdown();
  std::filesystem::remove(db_path);
}

} // namespace

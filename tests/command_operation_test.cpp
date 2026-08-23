#include "bridge_bot_operations.hpp"
#include "bridge_state_repository.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
#include "core/blocking_executor.hpp"
#include "core/db_manager.hpp"
#include "qq/command_handler.hpp"
#include "qq/event_handler.hpp"
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

  auto execute(const obcx::bot::EditTelegramMessageTextRequest &request)
      -> asio::awaitable<obcx::bot::BotOperationResult<
          obcx::bot::EditMessageTextResult>> override {
    edits.push_back(request);
    co_return obcx::bot::BotOperationResult<
        obcx::bot::EditMessageTextResult>::success({.message =
                                                        request.message});
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
  std::vector<obcx::bot::EditTelegramMessageTextRequest> edits;
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
      .source_installation = "tg-main",
      .source_platform = "telegram",
      .source_conversation_id = "chat:tg-group",
      .source_message_id = "100",
      .target_installation = "qq-main",
      .target_platform = "qq",
      .target_conversation_id = "group:qq-group",
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
  EXPECT_TRUE(
      repository
          ->resolve_target_mapping({.installation_id = "tg-main",
                                    .platform = "telegram",
                                    .conversation_id = "chat:tg-group",
                                    .message_id = "100"},
                                   {.installation_id = "qq-main",
                                    .platform = "qq",
                                    .conversation_id = "group:qq-group"})
          .missing());

  blocking->shutdown();
  std::filesystem::remove(db_path);
}

TEST(BridgeCommandOperationTest,
     RecallWithCollidingIdsUsesEachPairsExactInstallations) {
  const auto db_path = temp_db_path("recall-collisions");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping({
      .source_installation = "tg-a",
      .source_platform = "telegram",
      .source_conversation_id = "chat:tg-group",
      .source_message_id = "100",
      .target_installation = "qq-a",
      .target_platform = "qq",
      .target_conversation_id = "group:qq-group",
      .target_message_id = "target-a",
      .created_at = std::chrono::system_clock::now(),
  }));
  ASSERT_TRUE(repository->add_message_mapping({
      .source_installation = "tg-b",
      .source_platform = "telegram",
      .source_conversation_id = "chat:tg-group",
      .source_message_id = "100",
      .target_installation = "qq-b",
      .target_platform = "qq",
      .target_conversation_id = "group:qq-group",
      .target_message_id = "target-b",
      .created_at = std::chrono::system_clock::now(),
  }));

  auto client = std::make_shared<RecordingOperationClient>();
  auto first = std::make_shared<bridge::BridgeBotOperations>(client, "tg-a",
                                                             "qq-a", "a");
  auto second = std::make_shared<bridge::BridgeBotOperations>(client, "tg-b",
                                                              "qq-b", "b");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::telegram::TelegramCommandHandler first_handler(first, repository,
                                                         nullptr, blocking);
  bridge::telegram::TelegramCommandHandler second_handler(second, repository,
                                                          nullptr, blocking);
  obcx::common::MessageEvent first_event;
  first_event.group_id = "tg-group";
  first_event.message_id = "command-a";
  first_event.data = {{"reply_to_message", {{"message_id", 100}}}};
  auto second_event = first_event;
  second_event.message_id = "command-b";

  run(first_handler.handle_recall_command(std::move(first_event), "qq-group"));
  run(second_handler.handle_recall_command(std::move(second_event),
                                           "qq-group"));

  ASSERT_EQ(client->deletions.size(), 2U);
  EXPECT_EQ(client->deletions[0].message.group.installation.installation_id,
            "qq-a");
  EXPECT_EQ(client->deletions[0].message.native_message_id, "target-a");
  EXPECT_EQ(client->deletions[1].message.group.installation.installation_id,
            "qq-b");
  EXPECT_EQ(client->deletions[1].message.native_message_id, "target-b");
  EXPECT_TRUE(
      repository
          ->resolve_target_mapping({.installation_id = "tg-a",
                                    .platform = "telegram",
                                    .conversation_id = "chat:tg-group",
                                    .message_id = "100"},
                                   {.installation_id = "qq-a",
                                    .platform = "qq",
                                    .conversation_id = "group:qq-group"})
          .missing());
  EXPECT_TRUE(
      repository
          ->resolve_target_mapping({.installation_id = "tg-b",
                                    .platform = "telegram",
                                    .conversation_id = "chat:tg-group",
                                    .message_id = "100"},
                                   {.installation_id = "qq-b",
                                    .platform = "qq",
                                    .conversation_id = "group:qq-group"})
          .missing());
  blocking->shutdown();
  std::filesystem::remove(db_path);
}

TEST(BridgeCommandOperationTest,
     RecallResolvesEqualTelegramIdInCurrentConversation) {
  const auto db_path = temp_db_path("recall-chat-collision");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-current",
       .source_message_id = "1162814500",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-current",
       .target_message_id = "2700",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-history",
       .source_message_id = "-1583402916",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-history",
       .target_message_id = "2700",
       .created_at = std::chrono::system_clock::now()}));
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::telegram::TelegramCommandHandler handler(operations, repository,
                                                   nullptr, blocking);
  obcx::common::MessageEvent event;
  event.group_id = "tg-current";
  event.message_id = "command";
  event.data = {{"reply_to_message", {{"message_id", 2700}}}};

  run(handler.handle_recall_command(std::move(event), "qq-current"));

  ASSERT_EQ(client->deletions.size(), 1U);
  EXPECT_EQ(client->deletions.front().message.native_message_id, "1162814500");
  EXPECT_EQ(client->deletions.front().message.group.native_group_id,
            "qq-current");
  blocking->shutdown();
  std::filesystem::remove(db_path);
}

TEST(BridgeCommandOperationTest, AmbiguousRecallPerformsNoProviderCall) {
  const auto db_path = temp_db_path("recall-ambiguous");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  for (const auto source : {"qq-one", "qq-two"}) {
    ASSERT_TRUE(repository->add_message_mapping(
        {.source_installation = "qq-main",
         .source_platform = "qq",
         .source_conversation_id = "group:qq-current",
         .source_message_id = source,
         .target_installation = "tg-main",
         .target_platform = "telegram",
         .target_conversation_id = "chat:tg-current",
         .target_message_id = "2700",
         .is_primary = true,
         .created_at = std::chrono::system_clock::now()}));
  }
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  auto received = std::make_shared<bridge::ReceivedMessageRepository>(
      *db, "main", "message_store");
  bridge::telegram::TelegramCommandHandler handler(operations, repository,
                                                   received, blocking);
  obcx::common::MessageEvent event;
  event.group_id = "tg-current";
  event.message_id = "command";
  event.data = {{"reply_to_message", {{"message_id", 2700}}}};
  auto poke_event = event;

  run(handler.handle_recall_command(std::move(event), "qq-current"));
  run(handler.handle_poke_command(std::move(poke_event), "qq-current"));

  EXPECT_TRUE(client->deletions.empty());
  EXPECT_TRUE(client->pokes.empty());
  EXPECT_TRUE(client->sends.empty());
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
  ASSERT_TRUE(repository->update_platform_heartbeat("qq-main", "qq", now));
  ASSERT_TRUE(
      repository->update_platform_heartbeat("tg-main", "telegram", now));
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

TEST(BridgeCommandOperationTest, GeneratedRecallTextSanitizesControls) {
  const auto db_path = temp_db_path("recall-text-controls");
  auto db = std::make_shared<obcx::core::DbManager>();
  db->configure({sqlite_config(db_path)});
  auto repository =
      std::make_shared<bridge::BridgeStateRepository>(*db, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-group",
       .source_message_id = "qq-message",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-group",
       .target_message_id = "2700",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-other",
       .source_message_id = "qq-message",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-other",
       .target_message_id = "9999",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-group",
       .source_message_id = "qq-picture",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-group",
       .target_message_id = "2701",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "qq-main",
       .source_platform = "qq",
       .source_conversation_id = "group:qq-group",
       .source_message_id = "qq-hidden",
       .target_installation = "tg-main",
       .target_platform = "telegram",
       .target_conversation_id = "chat:tg-group",
       .target_message_id = "2702",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->save_or_update_user(
      {.installation_id = "qq-main",
       .platform = "qq",
       .user_id = "user",
       .group_id = "qq-group",
       .nickname = std::string{"bad\tname\r"} + static_cast<char>(1),
       .last_updated = std::chrono::system_clock::now()},
      true));
  db->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute(R"(
      CREATE TABLE message_store_qq_messages (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        message_id TEXT NOT NULL, source_platform TEXT NOT NULL,
        source_bot TEXT NOT NULL DEFAULT '', conversation_id TEXT NOT NULL,
        sender TEXT NOT NULL DEFAULT '', group_id TEXT NOT NULL DEFAULT '',
        message_type TEXT NOT NULL DEFAULT 'unknown',
        payload TEXT NOT NULL DEFAULT '{}', raw TEXT NOT NULL DEFAULT '{}',
        timestamp INTEGER NOT NULL, created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        UNIQUE(source_platform, source_bot, conversation_id, message_id));
    )");
    connection.execute(R"(
      INSERT INTO message_store_qq_messages
        (message_id, source_platform, source_bot, conversation_id, sender,
         group_id, message_type, payload, raw, timestamp, created_at,
         updated_at)
      VALUES ('qq-message','qq','qq-main','group:qq-group','user','qq-group',
              'group','{}','{"message":[{"type":"text"}]}',1,1,1),
             ('qq-picture','qq','qq-main','group:qq-group','user','qq-group',
              'image','{}','{"message":[{"type":"image"}]}',1,1,1),
             ('qq-hidden','qq','qq-main','group:qq-group','user','qq-group',
              'group','{}','{"message":[{"type":"text"}]}',1,1,1);
    )");
  });
  auto received = std::make_shared<bridge::ReceivedMessageRepository>(
      *db, "main", "message_store");
  auto client = std::make_shared<RecordingOperationClient>();
  auto operations = std::make_shared<bridge::BridgeBotOperations>(
      client, "tg-main", "qq-main");
  auto config = std::make_shared<bridge::BridgeConfig>();
  config->group_map.emplace("tg-group",
                            bridge::GroupBridgeConfig("tg-group", "qq-group",
                                                      true, false, true, true));
  config->group_map.emplace("tg-other",
                            bridge::GroupBridgeConfig("tg-other", "qq-other",
                                                      true, false, true, true));
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::qq::QQEventHandler handler(operations, config, repository, received,
                                     blocking);
  obcx::common::NoticeEvent notice;
  notice.notice_type = "group_recall";
  notice.group_id = "qq-group";
  notice.data = {{"message_id", "qq-message"}};
  obcx::common::Event event = notice;

  run(handler.handle_recall_event(std::move(event)));

  ASSERT_EQ(client->edits.size(), 1U);
  const auto &text = client->edits.front().text;
  EXPECT_EQ(text.find('\t'), std::string::npos);
  EXPECT_EQ(text.find('\r'), std::string::npos);
  EXPECT_EQ(text.find(static_cast<char>(1)), std::string::npos);
  EXPECT_NE(text.find("Message has been recalled"), std::string::npos);
  EXPECT_EQ(client->edits.front().message.group.native_group_id, "tg-group");
  EXPECT_EQ(client->edits.front().message.native_message_id, "2700");

  obcx::common::NoticeEvent picture;
  picture.notice_type = "group_recall";
  picture.group_id = "qq-group";
  picture.data = {{"message_id", "qq-picture"}};
  obcx::common::Event picture_event = picture;
  run(handler.handle_recall_event(std::move(picture_event)));
  ASSERT_EQ(client->deletions.size(), 1U);
  EXPECT_EQ(client->deletions.front().message.native_message_id, "2701");

  config->group_map.at("tg-group").show_qq_to_tg_sender = false;
  obcx::common::NoticeEvent hidden;
  hidden.notice_type = "group_recall";
  hidden.group_id = "qq-group";
  hidden.data = {{"message_id", "qq-hidden"}};
  obcx::common::Event hidden_event = hidden;
  run(handler.handle_recall_event(std::move(hidden_event)));
  ASSERT_EQ(client->edits.size(), 2U);
  EXPECT_EQ(client->edits.back().text, "~Message has been recalled~");

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
          INSERT INTO message_store_telegram_messages
            (message_id, source_platform, source_bot, conversation_id,
             sender, group_id, message_type, payload, raw, timestamp,
             created_at, updated_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )",
        {std::string{"100"}, std::string{"telegram"}, std::string{"tg-main"},
         std::string{"chat:tg-group"}, std::string{"qq-user"},
         std::string{"tg-group"}, std::string{"group"}, std::string{"{}"},
         std::string{"{}"}, std::int64_t{1}, std::int64_t{1}, std::int64_t{1}});
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

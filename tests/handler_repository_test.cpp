#include "bridge_state_repository.hpp"
#include "bridge_storage_models.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
#include "core/blocking_executor.hpp"
#include "core/db_manager.hpp"
#include "qq/message_formatter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class NoopBotOperationClient final : public obcx::bot::BotOperationClient {
public:
  auto supported_actions(const obcx::bot::BotInstallationRef &installation)
      const -> obcx::bot::BotOperationResult<
          obcx::bot::SupportedBotActions> override {
    return obcx::bot::BotOperationResult<
        obcx::bot::SupportedBotActions>::success({.installation =
                                                      installation});
  }
};

auto noop_bridge_operations() -> std::shared_ptr<bridge::BridgeBotOperations> {
  return std::make_shared<bridge::BridgeBotOperations>(
      std::make_shared<NoopBotOperationClient>(), "tg-main", "qq-main");
}

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("obcx_handler_repo_" + name + "_" + std::to_string(stamp) +
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

auto bridge_config_view(const std::filesystem::path &path,
                        const bool inject_default_bots = true)
    -> obcx::common::ActorConfigView {
  auto resolved = path;
  if (inject_default_bots) {
    std::ifstream input(path);
    const std::string content{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    if (!content.contains("[bots.")) {
      resolved += ".with-bots.toml";
      std::ofstream output(resolved);
      output << R"(
[bots.qq-main]
type = "qq"
enabled = true

[bots.tg-main]
type = "telegram"
enabled = true

)" << content;
    }
  }
  auto built = obcx::common::ConfigLoader::build_snapshot(resolved.string());
  if (resolved != path) {
    std::filesystem::remove(resolved);
  }
  if (!built) {
    throw std::runtime_error("failed to build test config snapshot");
  }
  return {std::move(built.snapshot), "bridge"};
}

} // namespace

TEST(BridgeHandlerRepositoryTest, DetectsActorPipelineOwnershipFromConfig) {
  const auto config_path =
      temp_db_path("actor_pipeline").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[actors.message_store]
library = "message_store"
enabled = true

[actors.bridge]
library = "bridge"
enabled = true

[pipelines.received_message]
source = "obcx::core::events::RawMessageEvent"

[[pipelines.received_message.stages]]
name = "persist_received"
actor = "message_store"
input = "obcx::core::events::RawMessageEvent"
output = "obcx::message_store::events::MessageStored"
mode = "await"

[[pipelines.received_message.stages]]
name = "forward"
actor = "bridge"
input = "obcx::message_store::events::MessageStored"
output = ["bridge::events::MessageForwarded", "bridge::events::MessageForwardFailed"]
after = ["persist_received"]
mode = "await"
)";
  }

  EXPECT_TRUE(bridge::actor_pipeline_enabled(bridge_config_view(config_path)));

  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, LoadsFfmpegPathFromActorConfiguration) {
  const auto config_path =
      temp_db_path("actor_ffmpeg_path").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
ffmpeg_path = "/opt/bridge/bin/ffmpeg"
)";
  }

  const auto config =
      bridge::load_bridge_config(bridge_config_view(config_path));
  EXPECT_EQ(config->ffmpeg_path, "/opt/bridge/bin/ffmpeg");

  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, LoadsOneExplicitEnabledInstallationPair) {
  const auto config_path =
      temp_db_path("explicit_installations").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[bots.qq-main]
type = "qq"
enabled = true

[bots.tg-main]
type = "telegram"
enabled = true

[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
)";
  }

  const auto config = bridge::load_bridge_config(
      bridge_config_view(config_path, /*inject_default_bots=*/false));
  EXPECT_EQ(config->telegram_installation, "tg-main");
  EXPECT_EQ(config->onebot11_installation, "qq-main");
  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, LoadsNamedPairsAndIsolatesCollidingRoutes) {
  const auto config_path =
      temp_db_path("named_installations").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[bots.qq-a]
type = "qq"
enabled = true
[bots.tg-a]
type = "telegram"
enabled = true
[bots.qq-b]
type = "qq"
enabled = true
[bots.tg-b]
type = "telegram"
enabled = true

[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
legacy_state_pair = "primary"

[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"

[[actors.bridge.config.installation_pairs]]
id = "secondary"
telegram_installation = "tg-b"
onebot11_installation = "qq-b"

[[actors.bridge.config.legacy_mapping_routes]]
pair = "primary"
telegram_group_id = "old-tg"
qq_group_id = "old-qq"

[[group_mappings.group_to_group]]
pair = "primary"
telegram_group_id = "same-tg-group"
qq_group_id = "same-qq-group"

[[group_mappings.group_to_group]]
pair = "secondary"
telegram_group_id = "same-tg-group"
qq_group_id = "same-qq-group"
)";
  }

  const auto config = bridge::load_bridge_config(
      bridge_config_view(config_path, /*inject_default_bots=*/false));
  ASSERT_EQ(config->installation_pairs.size(), 2U);
  ASSERT_NE(config->pair("primary"), nullptr);
  ASSERT_NE(config->pair("secondary"), nullptr);
  EXPECT_EQ(config->pair_for_source("telegram", "tg-a")->id, "primary");
  EXPECT_EQ(config->pair_for_source("qq", "qq-b")->id, "secondary");
  EXPECT_EQ(config->tg_group_and_topic_id("primary", "same-qq-group").first,
            "same-tg-group");
  EXPECT_EQ(config->tg_group_and_topic_id("secondary", "same-qq-group").first,
            "same-tg-group");
  EXPECT_EQ(config->legacy_migration_pair()->id, "primary");
  ASSERT_EQ(config->legacy_mapping_routes.size(), 1U);
  EXPECT_EQ(config->legacy_mapping_routes.front().telegram_installation,
            "tg-a");
  EXPECT_EQ(config->legacy_mapping_routes.front().onebot11_installation,
            "qq-a");
  EXPECT_TRUE(config->telegram_installation.empty());
  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, LoadsMigrationPolicyAndRouteHistory) {
  const auto config_path =
      temp_db_path("migration_routes").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
legacy_unresolved_mapping_policy = "archive"

[[actors.bridge.config.legacy_mapping_routes]]
telegram_conversation_id = "chat:-1001"
qq_conversation_id = "group:1001"
telegram_topic_id = 9

[[group_mappings.group_to_group]]
telegram_group_id = "-2002"
qq_group_id = "2002"
)";
  }

  const auto config =
      bridge::load_bridge_config(bridge_config_view(config_path));
  EXPECT_EQ(config->legacy_unresolved_mapping_policy,
            bridge::LegacyUnresolvedMappingPolicy::Archive);
  ASSERT_EQ(config->legacy_mapping_routes.size(), 1U);
  EXPECT_EQ(config->legacy_mapping_routes.front().pair_id, "legacy");
  EXPECT_EQ(config->legacy_mapping_routes.front().telegram_conversation_id,
            "chat:-1001");
  EXPECT_EQ(config->legacy_mapping_routes.front().qq_conversation_id,
            "group:1001");
  const auto migration = config->migration_context(true);
  EXPECT_EQ(migration.conversation_routes.size(), 2U);
  EXPECT_EQ(migration.unresolved_mapping_policy,
            bridge::LegacyUnresolvedMappingPolicy::Archive);
  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, RejectsInvalidMigrationConfiguration) {
  const std::vector<std::string> invalid = {
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
legacy_unresolved_mapping_policy = "guess"
)",
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_conversation_id = "group:wrong"
qq_conversation_id = "group:1"
)",
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_group_id = "1"
telegram_conversation_id = "chat:1"
qq_group_id = "2"
)",
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_group_id = "1"
qq_group_id = "2"
telegram_topic_id = 0
)",
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_group_id = "1"
qq_group_id = "2"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_group_id = "1"
qq_group_id = "3"
)",
      R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.legacy_mapping_routes]]
telegram_group_id = "1"
qq_group_id = "2"
[[group_mappings.group_to_group]]
telegram_group_id = "1"
qq_group_id = "2"
)",
  };
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    const auto path = temp_db_path("invalid_migration_" + std::to_string(index))
                          .replace_extension(".toml");
    {
      std::ofstream config(path);
      config << invalid[index];
    }
    EXPECT_THROW((void)bridge::load_bridge_config(bridge_config_view(path)),
                 std::runtime_error)
        << index;
    std::filesystem::remove(path);
  }
}

TEST(BridgeHandlerRepositoryTest, RejectsAmbiguousNamedPairConfiguration) {
  const std::vector<std::string> invalid = {
      R"(
[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
)",
      R"(
[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.installation_pairs]]
id = "duplicate"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
[[actors.bridge.config.installation_pairs]]
id = "duplicate"
telegram_installation = "tg-b"
onebot11_installation = "qq-b"
)",
      R"(
[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
[[actors.bridge.config.installation_pairs]]
id = "secondary"
telegram_installation = "tg-a"
onebot11_installation = "qq-b"
)",
      R"(
[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
[[actors.bridge.config.installation_pairs]]
id = "secondary"
telegram_installation = "tg-b"
onebot11_installation = "qq-b"
[[group_mappings.group_to_group]]
telegram_group_id = "tg-group"
qq_group_id = "qq-group"
)",
      R"(
[actors.bridge.config]
bridge_files_dir = "/tmp/bridge_files"
[[actors.bridge.config.installation_pairs]]
id = "primary"
telegram_installation = "tg-a"
onebot11_installation = "qq-a"
[[group_mappings.group_to_group]]
pair = "missing"
telegram_group_id = "tg-group"
qq_group_id = "qq-group"
)",
  };
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    const auto path = temp_db_path("invalid_named_" + std::to_string(index))
                          .replace_extension(".toml");
    {
      std::ofstream config(path);
      config << invalid[index];
    }
    EXPECT_THROW((void)bridge::load_bridge_config(bridge_config_view(path)),
                 std::runtime_error)
        << index;
    std::filesystem::remove(path);
  }
}

TEST(BridgeHandlerRepositoryTest, RejectsInvalidInstallationPair) {
  const std::vector<std::string> invalid = {
      R"(
[bots.qq-main]
type = "qq"
enabled = true
[bots.tg-main]
type = "telegram"
enabled = true
[actors.bridge.config]
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
)",
      R"(
[bots.same]
type = "telegram"
enabled = true
[actors.bridge.config]
telegram_installation = "same"
onebot11_installation = "same"
bridge_files_dir = "/tmp/bridge_files"
)",
      R"(
[bots.qq-main]
type = "qq"
enabled = true
[bots.tg-main]
type = "qq"
enabled = true
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
)",
      R"(
[bots.qq-main]
type = "qq"
enabled = false
[bots.tg-main]
type = "telegram"
enabled = true
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
)",
      R"(
[bots.qq-main]
type = "qq"
enabled = true
[actors.bridge.config]
telegram_installation = "missing-tg"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
)",
  };

  for (std::size_t index = 0; index < invalid.size(); ++index) {
    const auto config_path =
        temp_db_path("invalid_installations_" + std::to_string(index))
            .replace_extension(".toml");
    {
      std::ofstream config(config_path);
      config << invalid[index];
    }
    EXPECT_THROW((void)bridge::load_bridge_config(bridge_config_view(
                     config_path, /*inject_default_bots=*/false)),
                 std::runtime_error)
        << "invalid case " << index;
    std::filesystem::remove(config_path);
  }
}

TEST(BridgeHandlerRepositoryTest, ExactSourceBotMustMatchConfiguredSide) {
  bridge::BridgeConfig config;
  config.installation_pairs.emplace(
      "primary",
      bridge::BridgeInstallationPair{.id = "primary",
                                     .telegram_installation = "tg-main",
                                     .onebot11_installation = "qq-main"});

  EXPECT_NO_THROW(
      bridge::validate_bridge_source(config, "telegram", "tg-main"));
  EXPECT_NO_THROW(bridge::validate_bridge_source(config, "qq", "qq-main"));
  EXPECT_THROW(bridge::validate_bridge_source(config, "telegram", ""),
               std::runtime_error);
  EXPECT_THROW(bridge::validate_bridge_source(config, "telegram", "tg-other"),
               std::runtime_error);
  EXPECT_THROW(bridge::validate_bridge_source(config, "qq", "qq-other"),
               std::runtime_error);
  EXPECT_THROW(
      bridge::validate_bridge_source(config, "discord", "discord-main"),
      std::runtime_error);
}

TEST(BridgeHandlerRepositoryTest, LoadsQqMediaDownloadLimit) {
  const auto default_path =
      temp_db_path("actor_default_qq_media_limit").replace_extension(".toml");
  {
    std::ofstream config(default_path);
    config << "[actors.bridge.config]\n"
              "telegram_installation = \"tg-main\"\n"
              "onebot11_installation = \"qq-main\"\n"
              "bridge_files_dir = \"/tmp/bridge_files\"\n";
  }
  const auto default_config =
      bridge::load_bridge_config(bridge_config_view(default_path));
  EXPECT_EQ(default_config->qq_media_download_max_bytes, 10U * 1024U * 1024U);
  std::filesystem::remove(default_path);

  const auto configured_path =
      temp_db_path("actor_qq_media_limit").replace_extension(".toml");
  {
    std::ofstream config(configured_path);
    config << "[actors.bridge.config]\n"
              "telegram_installation = \"tg-main\"\n"
              "onebot11_installation = \"qq-main\"\n"
              "bridge_files_dir = \"/tmp/bridge_files\"\n"
              "qq_media_download_max_bytes = 5242880\n";
  }
  const auto configured =
      bridge::load_bridge_config(bridge_config_view(configured_path));
  EXPECT_EQ(configured->qq_media_download_max_bytes, 5U * 1024U * 1024U);
  std::filesystem::remove(configured_path);
}

TEST(BridgeHandlerRepositoryTest, RejectsInvalidQqMediaDownloadLimit) {
  const std::vector<std::string> invalid_values = {
      "qq_media_download_max_bytes = 0\n",
      "qq_media_download_max_bytes = -1\n",
      "qq_media_download_max_bytes = 10485761\n",
  };

  for (std::size_t index = 0; index < invalid_values.size(); ++index) {
    const auto config_path =
        temp_db_path("invalid_qq_media_limit_" + std::to_string(index))
            .replace_extension(".toml");
    {
      std::ofstream config(config_path);
      config << "[actors.bridge.config]\n"
                "telegram_installation = \"tg-main\"\n"
                "onebot11_installation = \"qq-main\"\n"
                "bridge_files_dir = \"/tmp/bridge_files\"\n"
             << invalid_values[index];
    }
    EXPECT_THROW(
        (void)bridge::load_bridge_config(bridge_config_view(config_path)),
        std::runtime_error)
        << "invalid case " << index;
    std::filesystem::remove(config_path);
  }
}

TEST(BridgeHandlerRepositoryTest, LoadsConfiguredMessageRetryPolicy) {
  const auto config_path =
      temp_db_path("actor_retry_policy").replace_extension(".toml");
  {
    std::ofstream config(config_path);
    config << R"(
[actors.bridge.config]
telegram_installation = "tg-main"
onebot11_installation = "qq-main"
bridge_files_dir = "/tmp/bridge_files"
enable_retry_queue = true
message_retry_max_attempts = 7
message_retry_base_interval_sec = 3
retry_queue_check_interval_sec = 4
max_retry_interval_sec = 20
)";
  }

  const auto config =
      bridge::load_bridge_config(bridge_config_view(config_path));
  EXPECT_TRUE(config->enable_retry_queue);
  EXPECT_EQ(config->message_retry_max_attempts, 7);
  EXPECT_EQ(config->message_retry_base_interval_sec, 3);
  EXPECT_EQ(config->retry_queue_check_interval_sec, 4);
  EXPECT_EQ(config->max_retry_interval_sec, 20);

  std::filesystem::remove(config_path);
}

TEST(BridgeHandlerRepositoryTest, RejectsInvalidMessageRetryPolicy) {
  const std::vector<std::string> invalid_values = {
      "message_retry_max_attempts = 0\n",
      "message_retry_base_interval_sec = 0\n",
      "retry_queue_check_interval_sec = -1\n",
      "max_retry_interval_sec = 0\n",
      "message_retry_base_interval_sec = 11\nmax_retry_interval_sec = 10\n",
      "retry_queue_check_interval_sec = 11\nmax_retry_interval_sec = 10\n",
  };

  for (std::size_t index = 0; index < invalid_values.size(); ++index) {
    const auto config_path =
        temp_db_path("invalid_retry_policy_" + std::to_string(index))
            .replace_extension(".toml");
    {
      std::ofstream config(config_path);
      config << "[actors.bridge.config]\n"
                "telegram_installation = \"tg-main\"\n"
                "onebot11_installation = \"qq-main\"\n"
                "bridge_files_dir = \"/tmp/bridge_files\"\n"
             << invalid_values[index];
    }

    EXPECT_THROW(
        (void)bridge::load_bridge_config(bridge_config_view(config_path)),
        std::runtime_error)
        << "invalid case " << index;
    std::filesystem::remove(config_path);
  }
}

TEST(BridgeHandlerRepositoryTest,
     ReverseLookupIgnoresMappingsWithQqToTelegramDisabled) {
  bridge::BridgeConfig config;
  config.group_map.emplace("tg-inbound-only",
                           bridge::GroupBridgeConfig("tg-inbound-only",
                                                     "qq-shared", true, true,
                                                     false, true));
  config.group_map.emplace("tg-bidirectional",
                           bridge::GroupBridgeConfig("tg-bidirectional",
                                                     "qq-shared", true, true,
                                                     true, true));

  const auto [telegram_group_id, topic_id] =
      config.tg_group_and_topic_id("legacy", "qq-shared");

  EXPECT_EQ(telegram_group_id, "tg-bidirectional");
  EXPECT_EQ(topic_id, -1);
}

TEST(BridgeHandlerRepositoryTest, QQReplyLookupUsesBridgeStateRepository) {
  const auto bridge_db_path = temp_db_path("bridge_mapping");

  auto bridge_db_manager = std::make_shared<obcx::core::DbManager>();
  bridge_db_manager->configure({sqlite_config(bridge_db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *bridge_db_manager, "main", "bridge");
  repository->initialize_schema();

  const storage::MessageMapping mapping{
      .source_installation = "qq-main",
      .source_platform = "qq",
      .source_conversation_id = "group:qq-group",
      .source_message_id = "qq-reply-1",
      .target_installation = "tg-main",
      .target_platform = "telegram",
      .target_conversation_id = "chat:tg-group",
      .target_message_id = "tg-reply-9",
      .created_at = std::chrono::system_clock::now(),
  };
  ASSERT_TRUE(repository->add_message_mapping(mapping));

  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  auto config = std::make_shared<bridge::BridgeConfig>();
  bridge::BridgeInstallationPair pair{.id = "legacy",
                                      .telegram_installation = "tg-main",
                                      .onebot11_installation = "qq-main"};
  pair.group_map.emplace("tg-group",
                         bridge::GroupBridgeConfig("tg-group", "qq-group"));
  config->installation_pairs.emplace("legacy", std::move(pair));
  bridge::qq::QQMessageFormatter formatter(noop_bridge_operations(), config,
                                           repository, blocking_executor);

  obcx::common::MessageEvent event;
  event.message_id = "qq-current";
  event.message_type = "group";
  event.group_id = "qq-group";
  obcx::common::MessageSegment reply;
  reply.type = "reply";
  reply.data["id"] = "qq-reply-1";
  event.message.push_back(std::move(reply));

  obcx::common::Message message_to_send;
  boost::asio::io_context ioc;
  auto future = boost::asio::co_spawn(
      ioc, formatter.format_reply_message(event, message_to_send),
      boost::asio::use_future);
  ioc.run();

  EXPECT_TRUE(future.get());
  blocking_executor->shutdown();
  ASSERT_EQ(message_to_send.size(), 1);
  EXPECT_EQ(message_to_send.front().type, "reply");
  EXPECT_EQ(message_to_send.front().data.at("id").get<std::string>(),
            "tg-reply-9");

  std::filesystem::remove(bridge_db_path);
}

TEST(BridgeHandlerRepositoryTest,
     QQReplyReverseLookupUsesCurrentConversationWithEqualTargetIds) {
  const auto bridge_db_path = temp_db_path("qq_reverse_collision");
  auto db_manager = std::make_shared<obcx::core::DbManager>();
  db_manager->configure({sqlite_config(bridge_db_path)});
  auto repository = std::make_shared<bridge::BridgeStateRepository>(
      *db_manager, "main", "bridge");
  repository->initialize_schema();
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "tg-main",
       .source_platform = "telegram",
       .source_conversation_id = "chat:tg-a",
       .source_message_id = "tg-source-a",
       .target_installation = "qq-main",
       .target_platform = "qq",
       .target_conversation_id = "group:qq-a",
       .target_message_id = "same-qq-id",
       .created_at = std::chrono::system_clock::now()}));
  ASSERT_TRUE(repository->add_message_mapping(
      {.source_installation = "tg-main",
       .source_platform = "telegram",
       .source_conversation_id = "chat:tg-b",
       .source_message_id = "tg-source-b",
       .target_installation = "qq-main",
       .target_platform = "qq",
       .target_conversation_id = "group:qq-b",
       .target_message_id = "same-qq-id",
       .created_at = std::chrono::system_clock::now()}));
  auto config = std::make_shared<bridge::BridgeConfig>();
  bridge::BridgeInstallationPair pair{.id = "legacy",
                                      .telegram_installation = "tg-main",
                                      .onebot11_installation = "qq-main"};
  pair.group_map.emplace("tg-a", bridge::GroupBridgeConfig("tg-a", "qq-a"));
  pair.group_map.emplace("tg-b", bridge::GroupBridgeConfig("tg-b", "qq-b"));
  config->installation_pairs.emplace("legacy", std::move(pair));
  auto blocking = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::qq::QQMessageFormatter formatter(noop_bridge_operations(), config,
                                           repository, blocking);
  obcx::common::MessageEvent event;
  event.message_type = "group";
  event.group_id = "qq-a";
  event.message.push_back({.type = "reply", .data = {{"id", "same-qq-id"}}});
  obcx::common::Message output;
  boost::asio::io_context ioc;
  auto future =
      boost::asio::co_spawn(ioc, formatter.format_reply_message(event, output),
                            boost::asio::use_future);
  ioc.run();

  EXPECT_TRUE(future.get());
  ASSERT_EQ(output.size(), 1U);
  EXPECT_EQ(output.front().data.value("id", std::string{}), "tg-source-a");
  blocking->shutdown();
  std::filesystem::remove(bridge_db_path);
}

TEST(BridgeHandlerRepositoryTest,
     ActorOnlyEntryPointOwnsForwardingWithoutRawPersistence) {
  const auto source_root = std::filesystem::path{OBCX_BRIDGE_SOURCE_DIR};
  const auto qq_handler = source_root / "dependency" / "qq" / "handler.cpp";
  const auto tg_handler =
      source_root / "dependency" / "telegram" / "handler.cpp";
  const auto actor = source_root / "actor" / "bridge_actor.cpp";
  const auto forwarding_runtime =
      source_root / "dependency" / "bridge_forwarding_runtime.cpp";

  auto read_file = [](const std::filesystem::path &path) {
    std::ifstream stream(path);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  };

  const auto qq_source = read_file(qq_handler);
  const auto tg_source = read_file(tg_handler);
  const auto actor_source = read_file(actor);
  const auto runtime_source = read_file(forwarding_runtime);

  EXPECT_EQ(qq_source.find("save_message_from_event"), std::string::npos);
  EXPECT_EQ(qq_source.find("save_user_from_event"), std::string::npos);
  EXPECT_EQ(tg_source.find("save_message_from_event"), std::string::npos);
  EXPECT_EQ(tg_source.find("save_user_from_event"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(source_root / "qq_to_tg" /
                                       "qq_to_tg_plugin.cpp"));
  EXPECT_FALSE(std::filesystem::exists(source_root / "tg_to_qq" /
                                       "tg_to_qq_plugin.cpp"));
  EXPECT_NE(actor_source.find("OBCX_ACTOR_EXPORT_V2"), std::string::npos);
  EXPECT_NE(actor_source.find("context.await_asio"), std::string::npos);
  EXPECT_NE(actor_source.find("obcx::message_store::events::MessageStored"),
            std::string::npos);
  EXPECT_NE(runtime_source.find("RetryQueueWorker"), std::string::npos);
  EXPECT_EQ(runtime_source.find("BotRegistry"), std::string::npos);
  EXPECT_NE(runtime_source.find("BotOperationClient"), std::string::npos);
  EXPECT_EQ(runtime_source.find("IPlugin"), std::string::npos);
  EXPECT_EQ(runtime_source.find("get_bots"), std::string::npos);
  EXPECT_NE(qq_source.find("消息发送失败且重试队列不可用"), std::string::npos);
  EXPECT_NE(qq_source.find("消息发送失败且未启用重试"), std::string::npos);
  EXPECT_NE(tg_source.find("消息发送失败且重试队列不可用"), std::string::npos);
  EXPECT_NE(tg_source.find("消息发送失败且未启用重试"), std::string::npos);
  EXPECT_EQ(qq_source.find("API响应: {}"), std::string::npos);
  EXPECT_EQ(tg_source.find("API响应: {}"), std::string::npos);
  EXPECT_EQ(qq_source.find("raw_message.starts_with(\"/\")"),
            std::string::npos);
  EXPECT_EQ(tg_source.find("raw_message.starts_with(\"/\")"),
            std::string::npos);
}

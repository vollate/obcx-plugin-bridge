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
  config.telegram_installation = "tg-main";
  config.onebot11_installation = "qq-main";

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
      config.tg_group_and_topic_id("qq-shared");

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
      .source_platform = "qq",
      .source_message_id = "qq-reply-1",
      .target_platform = "telegram",
      .target_message_id = "tg-reply-9",
      .created_at = std::chrono::system_clock::now(),
  };
  ASSERT_TRUE(repository->add_message_mapping(mapping));

  auto blocking_executor = std::make_shared<obcx::core::BlockingExecutor>(1);
  bridge::qq::QQMessageFormatter formatter(
      noop_bridge_operations(), std::make_shared<const bridge::BridgeConfig>(),
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

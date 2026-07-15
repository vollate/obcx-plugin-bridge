#include "bridge_state_repository.hpp"
#include "bridge_storage_models.hpp"
#include "common/config_loader.hpp"
#include "config.hpp"
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
#include <memory>
#include <sstream>
#include <string>

namespace {

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

  ASSERT_TRUE(
      obcx::common::ConfigLoader::instance().load_config(config_path.string()));
  EXPECT_TRUE(bridge::actor_pipeline_enabled());

  std::filesystem::remove(config_path);
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

  bridge::qq::QQMessageFormatter formatter(repository);

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

  auto read_file = [](const std::filesystem::path &path) {
    std::ifstream stream(path);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  };

  const auto qq_source = read_file(qq_handler);
  const auto tg_source = read_file(tg_handler);
  const auto actor_source = read_file(actor);

  EXPECT_EQ(qq_source.find("save_message_from_event"), std::string::npos);
  EXPECT_EQ(qq_source.find("save_user_from_event"), std::string::npos);
  EXPECT_EQ(tg_source.find("save_message_from_event"), std::string::npos);
  EXPECT_EQ(tg_source.find("save_user_from_event"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(
      source_root / "qq_to_tg" / "qq_to_tg_plugin.cpp"));
  EXPECT_FALSE(std::filesystem::exists(
      source_root / "tg_to_qq" / "tg_to_qq_plugin.cpp"));
  EXPECT_NE(actor_source.find("OBCX_ACTOR_EXPORT_V2"), std::string::npos);
  EXPECT_NE(actor_source.find("context.await_asio"), std::string::npos);
  EXPECT_NE(actor_source.find("obcx::message_store::events::MessageStored"), std::string::npos);
}

#include "config.hpp"
#include "database/manager.hpp"
#include "interfaces/bot.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "telegram/handler.hpp"

#include <boost/asio.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace asio = boost::asio;

namespace {

constexpr std::string_view kTelegramGroupId = "-114514";
constexpr std::string_view kQQGroupId = "2222";

auto make_temp_db_path() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("obcx_bridge_media_group_mapping_" + std::to_string(::getpid()) +
          ".sqlite3");
}

class FakeBot final : public obcx::core::IBot {
public:
  FakeBot()
      : IBot(std::make_unique<obcx::adapter::onebot11::ProtocolAdapter>()) {}

  void run() override {}
  void stop() override {}
  void error_notify(std::string_view, std::string_view, bool) override {}

  auto send_private_message(std::string_view, const obcx::common::Message &)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }

  auto send_group_message(std::string_view group_id,
                          const obcx::common::Message &message)
      -> asio::awaitable<std::string> override {
    sent_group_ids.emplace_back(group_id);
    sent_messages.push_back(message);
    co_return R"({"status":"ok","data":{"message_id":4242}})";
  }

  auto delete_message(std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_message(std::string_view) -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_friend_list() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_stranger_info(std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_group_list() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_group_info(std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_group_member_list(std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_group_member_info(std::string_view, std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_kick(std::string_view, std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_ban(std::string_view, std::string_view, int32_t)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_whole_ban(std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_card(std::string_view, std::string_view, std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_leave(std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_name(std::string_view, std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_admin(std::string_view, std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_anonymous_ban(std::string_view, const std::string &, int32_t)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_anonymous(std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_portrait(std::string_view, std::string_view, bool)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_group_honor_info(std::string_view, std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_login_info() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_status() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_version_info() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_image(std::string_view) -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_record(std::string_view, std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto can_send_image() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto can_send_record() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_cookies(std::string_view) -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_csrf_token() -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto get_credentials(std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_friend_add_request(std::string_view, bool, std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }
  auto set_group_add_request(std::string_view, std::string_view, bool,
                             std::string_view)
      -> asio::awaitable<std::string> override {
    co_return R"({"status":"ok"})";
  }

  [[nodiscard]] auto is_connected() const -> bool override { return true; }
  auto get_task_scheduler() -> obcx::core::TaskScheduler & override {
    return *task_scheduler_;
  }

  std::vector<std::string> sent_group_ids;
  std::vector<obcx::common::Message> sent_messages;

private:
  void connect(obcx::network::ConnectionManagerFactory::ConnectionType,
               const obcx::common::ConnectionConfig &) override {}
};

auto make_album_event(std::string message_id) -> obcx::common::MessageEvent {
  obcx::common::MessageEvent event;
  event.type = obcx::common::EventType::message;
  event.post_type = "message";
  event.message_type = "group";
  event.message_id = std::move(message_id);
  event.user_id = "777";
  event.group_id = std::string{kTelegramGroupId};
  event.raw_message = "album item";
  event.data["media_group_id"] = "album-1";

  obcx::common::MessageSegment segment;
  segment.type = "text";
  segment.data["text"] = "album item";
  event.message.push_back(std::move(segment));
  return event;
}

} // namespace

TEST(TGMediaGroupMappingTest, mapsEveryAlbumMessageIdToSameQQMessageId) {
  const auto db_path = make_temp_db_path();
  std::filesystem::remove(db_path);
  storage::DatabaseManager::reset_instance();
  auto db_manager = storage::DatabaseManager::instance(db_path.string());
  ASSERT_TRUE(db_manager->initialize());

  bridge::GROUP_MAP.clear();
  bridge::GROUP_MAP.emplace(
      std::string{kTelegramGroupId},
      bridge::GroupBridgeConfig{std::string{kTelegramGroupId},
                                std::string{kQQGroupId}, true, true, true,
                                true});

  asio::io_context ioc;
  auto handler = std::make_shared<bridge::TelegramHandler>(db_manager, nullptr,
                                                           ioc.get_executor());
  FakeBot tg_bot;
  FakeBot qq_bot;

  asio::co_spawn(
      ioc,
      [&]() -> asio::awaitable<void> {
        co_await handler->forward_to_qq(tg_bot, qq_bot,
                                        make_album_event("1001"));
        co_await handler->forward_to_qq(tg_bot, qq_bot,
                                        make_album_event("1002"));
        co_await handler->forward_to_qq(tg_bot, qq_bot,
                                        make_album_event("1003"));
        handler->flush_pending_media_groups();
      },
      asio::detached);
  ioc.run();

  ASSERT_EQ(qq_bot.sent_messages.size(), 1U);
  EXPECT_EQ(qq_bot.sent_group_ids.front(), kQQGroupId);
  EXPECT_EQ(db_manager->get_target_message_id("telegram", "1001", "qq"),
            std::optional<std::string>{"4242"});
  EXPECT_EQ(db_manager->get_target_message_id("telegram", "1002", "qq"),
            std::optional<std::string>{"4242"});
  EXPECT_EQ(db_manager->get_target_message_id("telegram", "1003", "qq"),
            std::optional<std::string>{"4242"});

  bridge::GROUP_MAP.clear();
  storage::DatabaseManager::reset_instance();
  std::filesystem::remove(db_path);
}

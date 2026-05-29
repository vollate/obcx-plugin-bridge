#include "database/manager.hpp"
#include "qq/message_formatter.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <common/message_type.hpp>
#include <filesystem>
#include <gtest/gtest.h>

namespace {

auto make_db_manager(const std::filesystem::path &db_path)
    -> std::shared_ptr<storage::DatabaseManager> {
  storage::DatabaseManager::reset_instance();
  auto db_manager = storage::DatabaseManager::instance(db_path.string());
  EXPECT_NE(db_manager, nullptr);
  EXPECT_TRUE(db_manager->initialize());
  return db_manager;
}

} // namespace

TEST(QQMessageFormatterReplyTest, NumericReplyIdDoesNotThrow) {
  const auto db_path = std::filesystem::temp_directory_path() /
                       "obcx_qq_message_formatter_reply_test.sqlite3";
  std::filesystem::remove(db_path);

  auto db_manager = make_db_manager(db_path);
  bridge::qq::QQMessageFormatter formatter(db_manager);

  obcx::common::MessageEvent event;
  event.message_type = "group";
  event.message_id = "source-message";
  event.group_id = "12345";

  obcx::common::MessageSegment reply_segment;
  reply_segment.type = "reply";
  reply_segment.data["id"] = 123456789;
  event.message.push_back(reply_segment);

  obcx::common::Message message_to_send;
  boost::asio::io_context io_context;
  std::optional<bool> result;
  std::exception_ptr error;

  boost::asio::co_spawn(
      io_context,
      [&]() -> boost::asio::awaitable<void> {
        try {
          result =
              co_await formatter.format_reply_message(event, message_to_send);
        } catch (...) {
          error = std::current_exception();
        }
      },
      boost::asio::detached);

  io_context.run();

  EXPECT_EQ(error, nullptr);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result.value());
  EXPECT_TRUE(message_to_send.empty());

  storage::DatabaseManager::reset_instance();
  std::filesystem::remove(db_path);
}

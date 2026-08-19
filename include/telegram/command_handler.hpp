#pragma once

#include "bridge_bot_operations.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>
#include <string_view>

namespace bridge {
class BridgeStateRepository;
class ReceivedMessageRepository;
} // namespace bridge

namespace bridge::telegram {

class TelegramCommandHandler {
public:
  explicit TelegramCommandHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<bridge::ReceivedMessageRepository>
          received_message_repository = nullptr,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor =
          nullptr);

  auto handle_recall_command(obcx::common::MessageEvent event,
                             std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;
  auto handle_checkalive_command(obcx::common::MessageEvent event,
                                 std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;
  auto handle_poke_command(obcx::common::MessageEvent event,
                           std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;

private:
  auto send_reply_message(const std::string &telegram_group_id,
                          const std::string &reply_to_message_id,
                          const std::string &text)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<bridge::ReceivedMessageRepository>
      received_message_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
};

} // namespace bridge::telegram

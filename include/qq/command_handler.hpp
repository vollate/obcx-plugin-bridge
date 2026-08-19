#pragma once

#include "bridge_bot_operations.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>

namespace bridge {
class BridgeStateRepository;
}

namespace bridge::qq {

class QQCommandHandler {
public:
  explicit QQCommandHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor);

  auto handle_checkalive_command(obcx::common::MessageEvent event,
                                 const std::string &telegram_group_id)
      -> boost::asio::awaitable<void>;

private:
  auto send_reply_message(const std::string &qq_group_id,
                          const std::string &reply_to_message_id,
                          const std::string &text)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
};

} // namespace bridge::qq

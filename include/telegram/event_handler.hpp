#pragma once

#include "bridge_bot_operations.hpp"
#include "bridge_forwarder.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <functional>
#include <memory>

namespace bridge {
class BridgeStateRepository;
struct BridgeConfig;
} // namespace bridge

namespace bridge::telegram {

class TelegramEventHandler {
public:
  explicit TelegramEventHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const bridge::BridgeConfig> config,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository,
      std::function<boost::asio::awaitable<DirectForwardOutcome>(
          obcx::common::MessageEvent)>
          forward_function,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor);

  auto handle_message_deleted(obcx::common::Event event)
      -> boost::asio::awaitable<void>;
  auto handle_message_edited(obcx::common::MessageEvent event)
      -> boost::asio::awaitable<DirectForwardOutcome>;

private:
  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const bridge::BridgeConfig> config_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
  std::function<boost::asio::awaitable<DirectForwardOutcome>(
      obcx::common::MessageEvent)>
      forward_function_;
};

} // namespace bridge::telegram

#pragma once

#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"
#include "received_message_repository.hpp"

#include <core/bot_registry.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <memory>

namespace bridge {

class QQHandler;
class TelegramHandler;

class BridgeForwardingRuntime final : public IBridgeForwarder {
public:
  BridgeForwardingRuntime(
      std::shared_ptr<obcx::core::BotRegistry> bot_registry,
      std::shared_ptr<BridgeStateRepository> state_repository,
      std::shared_ptr<ReceivedMessageRepository> received_message_repository,
      boost::asio::any_io_executor buffer_executor);

  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> boost::asio::awaitable<BridgeForwardResult> override;

private:
  std::shared_ptr<obcx::core::BotRegistry> bot_registry_;
  std::shared_ptr<BridgeStateRepository> state_repository_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
  std::shared_ptr<QQHandler> qq_handler_;
  std::shared_ptr<TelegramHandler> telegram_handler_;
};

} // namespace bridge

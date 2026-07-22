#pragma once

#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"
#include "received_message_repository.hpp"

#include <core/bot_registry.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>

namespace bridge {

class QQHandler;
class RetryQueueWorker;
class TelegramHandler;
struct BridgeConfig;

class BridgeRetryUnavailable final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class BridgeForwardingRuntime final : public IBridgeForwarder {
public:
  BridgeForwardingRuntime(
      std::shared_ptr<obcx::core::BotRegistry> bot_registry,
      std::shared_ptr<const BridgeConfig> config,
      std::shared_ptr<BridgeStateRepository> state_repository,
      std::shared_ptr<ReceivedMessageRepository> received_message_repository,
      boost::asio::any_io_executor buffer_executor);
  ~BridgeForwardingRuntime() override;

  auto forward_message(const obcx::core::MessageEnvelope &message)
      -> boost::asio::awaitable<BridgeForwardResult> override;

  void shutdown() noexcept;

private:
  std::shared_ptr<obcx::core::BotRegistry> bot_registry_;
  std::shared_ptr<const BridgeConfig> config_;
  std::shared_ptr<BridgeStateRepository> state_repository_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
  std::unique_ptr<RetryQueueWorker> retry_worker_;
  std::shared_ptr<QQHandler> qq_handler_;
  std::shared_ptr<TelegramHandler> telegram_handler_;
  std::atomic_bool shutting_down_{false};
};

} // namespace bridge

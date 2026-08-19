#pragma once

#include "bridge_bot_operations.hpp"
#include "bridge_forwarder.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>

namespace bridge {

class BridgeStateRepository;
struct BridgeConfig;
class ReceivedMessageRepository;
class RetryQueueManager;

namespace qq {
class QQMediaProcessor;
class QQCommandHandler;
class QQEventHandler;
class QQMessageFormatter;
} // namespace qq

class QQHandler {
public:
  explicit QQHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const BridgeConfig> config,
      std::shared_ptr<RetryQueueManager> retry_manager = nullptr,
      std::shared_ptr<BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<ReceivedMessageRepository> received_message_repository =
          nullptr,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor =
          nullptr);
  ~QQHandler();

  auto forward_to_telegram(obcx::common::MessageEvent event)
      -> boost::asio::awaitable<DirectForwardOutcome>;
  auto handle_recall_event(obcx::common::Event event)
      -> boost::asio::awaitable<void>;
  auto handle_checkalive_command(obcx::common::MessageEvent event,
                                 const std::string &telegram_group_id)
      -> boost::asio::awaitable<void>;
  auto handle_poke_event(const obcx::common::NoticeEvent &event)
      -> boost::asio::awaitable<void>;

private:
  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const BridgeConfig> config_;
  std::shared_ptr<RetryQueueManager> retry_manager_;
  std::shared_ptr<BridgeStateRepository> state_repository_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
  std::unique_ptr<qq::QQMediaProcessor> media_processor_;
  std::unique_ptr<qq::QQCommandHandler> command_handler_;
  std::unique_ptr<qq::QQEventHandler> event_handler_;
  std::unique_ptr<qq::QQMessageFormatter> message_formatter_;
};

} // namespace bridge

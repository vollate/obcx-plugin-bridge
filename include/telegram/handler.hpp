#pragma once

#include "bridge_bot_operations.hpp"
#include "bridge_forwarder.hpp"
#include "common/message_type.hpp"
#include "telegram/command_handler.hpp"
#include "telegram/event_handler.hpp"
#include "telegram/media_processor.hpp"

#include <boost/asio.hpp>
#include <core/blocking_executor.hpp>
#include <memory>

namespace bridge {

class BridgeStateRepository;
struct BridgeConfig;
class ReceivedMessageRepository;
class RetryQueueManager;

namespace telegram {
class TGMediaGroupBuffer;
} // namespace telegram

class TelegramHandler : public std::enable_shared_from_this<TelegramHandler> {
public:
  explicit TelegramHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const BridgeConfig> config,
      std::shared_ptr<RetryQueueManager> retry_manager,
      boost::asio::any_io_executor buffer_executor,
      std::shared_ptr<BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<ReceivedMessageRepository> received_message_repository =
          nullptr,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor =
          nullptr);

  auto forward_to_qq(obcx::common::MessageEvent event)
      -> boost::asio::awaitable<DirectForwardOutcome>;
  auto handle_message_deleted(obcx::common::Event event)
      -> boost::asio::awaitable<void>;
  auto handle_message_edited(obcx::common::MessageEvent event)
      -> boost::asio::awaitable<DirectForwardOutcome>;
  auto handle_recall_command(obcx::common::MessageEvent event,
                             std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;
  auto handle_checkalive_command(obcx::common::MessageEvent event,
                                 std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;
  auto handle_poke_command(obcx::common::MessageEvent event,
                           std::string_view qq_group_id)
      -> boost::asio::awaitable<void>;

  void flush_pending_media_groups();

private:
  auto forward_media_group_to_qq(std::vector<obcx::common::MessageEvent> events)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const BridgeConfig> config_;
  std::shared_ptr<RetryQueueManager> retry_manager_;
  std::shared_ptr<BridgeStateRepository> state_repository_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
  std::unique_ptr<telegram::TelegramMediaProcessor> media_processor_;
  std::unique_ptr<telegram::TelegramCommandHandler> command_handler_;
  std::unique_ptr<telegram::TelegramEventHandler> event_handler_;
  std::shared_ptr<telegram::TGMediaGroupBuffer> media_group_buffer_;
  boost::asio::any_io_executor buffer_executor_;
};

} // namespace bridge

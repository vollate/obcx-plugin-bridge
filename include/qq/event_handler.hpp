#pragma once

#include "bridge_bot_operations.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>

namespace bridge {
class BridgeStateRepository;
struct BridgeConfig;
class ReceivedMessageRepository;
} // namespace bridge

namespace bridge::qq {

class QQEventHandler {
public:
  explicit QQEventHandler(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const bridge::BridgeConfig> config,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<bridge::ReceivedMessageRepository>
          received_message_repository = nullptr,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor =
          nullptr);

  auto handle_recall_event(obcx::common::Event event)
      -> boost::asio::awaitable<void>;
  auto handle_poke_event(const obcx::common::NoticeEvent &event)
      -> boost::asio::awaitable<void>;

private:
  static auto get_poke_action_name(int poke_type, int poke_id) -> std::string;
  static auto escape_markdown_v2(const std::string &text) -> std::string;
  auto fetch_user_display_name(const std::string &user_id,
                               const std::string &group_id)
      -> boost::asio::awaitable<std::string>;
  auto fetch_user_info(const std::string &user_id, const std::string &group_id)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const bridge::BridgeConfig> config_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<bridge::ReceivedMessageRepository>
      received_message_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
};

} // namespace bridge::qq

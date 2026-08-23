#pragma once

#include <core/actor.hpp>
#include <core/actor_commands.hpp>

#include <boost/asio/awaitable.hpp>

#include <string>

namespace bridge {

enum class DirectForwardDisposition {
  NotForwarded,
  DeliveryFailed,
  NewDelivery,
  AlreadyPersisted,
};

struct DirectForwardOutcome {
  DirectForwardDisposition disposition = DirectForwardDisposition::NotForwarded;
  std::string source_platform;
  std::string source_bot;
  std::string source_conversation_id;
  std::string source_message_id;
  std::string target_platform;
  std::string target_bot;
  std::string target_conversation_id;
  std::string target_message_id;
  std::string failure_message;
  bool failure_retryable = false;
};

struct BridgeForwardResult {
  DirectForwardDisposition disposition = DirectForwardDisposition::NotForwarded;
  std::string source_platform;
  std::string source_bot;
  std::string source_conversation_id;
  std::string source_message_id;
  std::string target_platform;
  std::string target_bot;
  std::string target_conversation_id;
  std::string target_message_id;
  std::string failure_message;
  bool failure_retryable = false;
};

class IBridgeForwarder {
public:
  IBridgeForwarder() = default;
  IBridgeForwarder(const IBridgeForwarder &) = delete;
  auto operator=(const IBridgeForwarder &) -> IBridgeForwarder & = delete;
  IBridgeForwarder(IBridgeForwarder &&) = delete;
  auto operator=(IBridgeForwarder &&) -> IBridgeForwarder & = delete;
  virtual ~IBridgeForwarder() = default;

  virtual auto forward_message(const obcx::core::MessageEnvelope &message)
      -> boost::asio::awaitable<BridgeForwardResult> = 0;

  virtual auto handle_command(const obcx::command::CommandInvocation &)
      -> boost::asio::awaitable<bool> {
    co_return false;
  }

  virtual auto handle_notice(const obcx::core::MessageEnvelope &)
      -> boost::asio::awaitable<bool> {
    co_return false;
  }
};

} // namespace bridge

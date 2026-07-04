#pragma once

#include <core/actor.hpp>

#include <boost/asio/awaitable.hpp>

#include <string>

namespace bridge {

struct BridgeForwardResult {
  std::string source_platform;
  std::string source_message_id;
  std::string target_platform;
  std::string target_bot;
  std::string target_message_id;
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
};

} // namespace bridge

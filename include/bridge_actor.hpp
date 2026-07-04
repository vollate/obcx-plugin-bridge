#pragma once

#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"

#include <core/actor.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>

namespace bridge {

class ReceivedMessageRepository;

} // namespace bridge

namespace bridge {

class BridgeActor final : public obcx::core::IActor {
public:
  BridgeActor() = default;

  [[nodiscard]] auto get_name() const -> std::string override;
  [[nodiscard]] auto get_version() const -> std::string override;

  auto handle_message(const obcx::core::MessageEnvelope &message,
                      obcx::core::ActorContext &context)
      -> boost::asio::awaitable<obcx::core::ActorResult> override;

private:
  auto resolve_repository(obcx::core::ActorContext &context)
      -> std::shared_ptr<BridgeStateRepository>;
  auto resolve_forwarder(obcx::core::ActorContext &context,
                         boost::asio::any_io_executor executor)
      -> std::shared_ptr<IBridgeForwarder>;

  std::shared_ptr<BridgeStateRepository> repository_;
  std::shared_ptr<IBridgeForwarder> forwarder_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
};

} // namespace bridge

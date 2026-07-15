#pragma once

#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"

#include <core/actor_messages.hpp>
#include <core/reflected_actor.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <memory>
#include <string>

namespace bridge {

class ReceivedMessageRepository;

} // namespace bridge

namespace bridge {

class BridgeActor final : public obcx::core::ReflectedActor<BridgeActor> {
public:
  static constexpr std::string_view actor_name = "bridge";
  static constexpr std::string_view actor_version = "0.1.0";

  BridgeActor() = default;

  auto handle(const obcx::message_store::events::MessageStored &stored,
              const obcx::core::MessageEnvelope &message,
              obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;

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

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
struct BridgeConfig;

} // namespace bridge

namespace bridge {

class BridgeActor final : public obcx::core::ReflectedActor<BridgeActor> {
public:
  static constexpr std::string_view actor_name = "bridge";
  static constexpr std::string_view actor_version = "0.1.0";

  [[nodiscard]] static auto configuration_contract() -> obcx::common::json {
    return {
        {"integers",
         {{"max_retry_interval_sec", {{"default", 300}, {"minimum", 1}}},
          {"message_retry_base_interval_sec", {{"default", 2}, {"minimum", 1}}},
          {"message_retry_max_attempts", {{"default", 5}, {"minimum", 1}}},
          {"retry_queue_check_interval_sec",
           {{"default", 10}, {"minimum", 1}}}}},
        {"less_equal",
         obcx::common::json::array(
             {obcx::common::json::array({"message_retry_base_interval_sec",
                                         "max_retry_interval_sec"}),
              obcx::common::json::array({"retry_queue_check_interval_sec",
                                         "max_retry_interval_sec"})})},
    };
  }

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
  auto resolve_config(obcx::core::ActorContext &context)
      -> std::shared_ptr<const BridgeConfig>;

  std::shared_ptr<const BridgeConfig> config_;
  std::shared_ptr<BridgeStateRepository> repository_;
  std::shared_ptr<IBridgeForwarder> forwarder_;
  std::shared_ptr<ReceivedMessageRepository> received_message_repository_;
};

} // namespace bridge

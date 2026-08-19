#pragma once

#include "bridge_forwarder.hpp"
#include "bridge_state_repository.hpp"

#include <core/actor_commands.hpp>
#include <core/actor_messages.hpp>
#include <core/reflected_actor.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace bridge {

class ReceivedMessageRepository;
struct BridgeConfig;

} // namespace bridge

namespace bridge {

namespace commands {
struct RecallCommand final : obcx::command::RequestMessage<RecallCommand> {};
struct CheckAliveCommand final
    : obcx::command::RequestMessage<CheckAliveCommand> {};
struct PokeCommand final : obcx::command::RequestMessage<PokeCommand> {};
} // namespace commands

class BridgeActor final : public obcx::core::ReflectedActor<BridgeActor> {
public:
  static constexpr std::string_view actor_name = "bridge";
  static constexpr std::string_view actor_version = "0.1.0";

  static constexpr auto command_contract() {
    return obcx::command::catalog(
        obcx::command::observe<commands::RecallCommand>(
            "recall", "Recall the replied bridged message"),
        obcx::command::observe<commands::CheckAliveCommand>(
            "checkalive", "Check the bridge platform connection"),
        obcx::command::observe<commands::PokeCommand>(
            "poke", "Poke the replied QQ user"));
  }

  [[nodiscard]] static auto configuration_contract() -> obcx::common::json {
    return {
        {"integers",
         {{"max_retry_interval_sec", {{"default", 300}, {"minimum", 1}}},
          {"message_retry_base_interval_sec", {{"default", 2}, {"minimum", 1}}},
          {"message_retry_max_attempts", {{"default", 5}, {"minimum", 1}}},
          {"retry_queue_check_interval_sec",
           {{"default", 10}, {"minimum", 1}}}}},
        {"required_strings", obcx::common::json::array({"bridge_files_dir"})},
        {"bot_installations",
         {{"onebot11_installation", "qq"},
          {"telegram_installation", "telegram"}}},
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
  auto handle(const obcx::core::events::RawNoticeEvent &notice,
              const obcx::core::MessageEnvelope &message,
              obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;
  auto handle(const commands::RecallCommand &request,
              const obcx::core::MessageEnvelope &message,
              obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;
  auto handle(const commands::CheckAliveCommand &request,
              const obcx::core::MessageEnvelope &message,
              obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;
  auto handle(const commands::PokeCommand &request,
              const obcx::core::MessageEnvelope &message,
              obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;

private:
  auto handle_command(const obcx::command::CommandInvocation &invocation,
                      const obcx::core::MessageEnvelope &message,
                      obcx::core::ActorContext &context)
      -> obcx::core::ActorTask<obcx::core::ActorResult>;
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
  std::recursive_mutex runtime_mutex_;
};

} // namespace bridge

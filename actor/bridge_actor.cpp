#include "bridge_actor.hpp"
#include "bridge_forwarder.hpp"
#include "bridge_forwarding_runtime.hpp"
#include "config.hpp"
#include "received_message_repository.hpp"

#include <common/json_utils.hpp>
#include <core/bot_registry.hpp>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace bridge {
namespace asio = boost::asio;
namespace {

auto payload_string(const obcx::common::json &payload, const char *key)
    -> std::string {
  if (!payload.contains(key) || payload.at(key).is_null()) {
    return {};
  }
  const auto &value = payload.at(key);
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<std::int64_t>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<std::uint64_t>());
  }
  return value.dump();
}

auto build_forwarded_event(const obcx::core::MessageEnvelope &message,
                           const storage::MessageMapping &mapping,
                           const std::string &target_bot)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id =
      "forwarded-" + mapping.source_platform + "-" + mapping.source_message_id;
  envelope.type = "bridge::events::MessageForwarded";
  envelope.source_platform = mapping.source_platform;
  envelope.source_bot = message.source_bot;
  envelope.correlation_id = message.correlation_id;
  envelope.causation_id = message.id;
  envelope.timestamp = std::chrono::system_clock::now();
  envelope.payload = {
      {"source_platform", mapping.source_platform},
      {"source_bot", message.source_bot},
      {"source_message_id", mapping.source_message_id},
      {"target_platform", mapping.target_platform},
      {"target_bot", target_bot},
      {"target_message_id", mapping.target_message_id},
  };
  return envelope;
}

auto mapping_from_forward_result(const BridgeForwardResult &forward_result)
    -> storage::MessageMapping {
  return storage::MessageMapping{
      .source_platform = forward_result.source_platform,
      .source_message_id = forward_result.source_message_id,
      .target_platform = forward_result.target_platform,
      .target_message_id = forward_result.target_message_id,
      .created_at = std::chrono::system_clock::now(),
  };
}

auto build_failed_event(const obcx::core::MessageEnvelope &message,
                        const std::string &code,
                        const std::string &error_message, const bool retryable)
    -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "forward-failed-" + message.id;
  envelope.type = "bridge::events::MessageForwardFailed";
  envelope.source_platform = message.source_platform;
  envelope.source_bot = message.source_bot;
  envelope.correlation_id = message.correlation_id;
  envelope.causation_id = message.id;
  envelope.timestamp = std::chrono::system_clock::now();
  envelope.payload = {
      {"code", code},
      {"message", error_message},
      {"retryable", retryable},
      {"source_platform", message.source_platform},
      {"source_message_id", payload_string(message.payload, "message_id")},
  };
  return envelope;
}

auto source_message_id(const obcx::core::MessageEnvelope &message)
    -> std::string {
  auto id = payload_string(message.payload, "source_message_id");
  if (!id.empty()) {
    return id;
  }
  return payload_string(message.payload, "message_id");
}

} // namespace

auto BridgeActor::resolve_repository(obcx::core::ActorContext &context)
    -> std::shared_ptr<BridgeStateRepository> {
  std::scoped_lock lock(runtime_mutex_);
  if (repository_) {
    return repository_;
  }
  if (auto injected = context.get_service<BridgeStateRepository>()) {
    repository_ = std::move(injected);
    return repository_;
  }

  auto db_manager = context.get_service<obcx::core::DbManager>();
  if (!db_manager) {
    throw std::runtime_error("bridge requires DbManager service");
  }

  auto db_instance = context.db_instance().empty() ? std::string{"main"}
                                                   : context.db_instance();
  auto db_namespace = context.db_namespace().empty() ? context.actor_id()
                                                     : context.db_namespace();
  repository_ = std::make_shared<BridgeStateRepository>(
      *db_manager, std::move(db_instance), std::move(db_namespace));
  repository_->initialize_schema();
  return repository_;
}

auto BridgeActor::resolve_config(obcx::core::ActorContext &context)
    -> std::shared_ptr<const BridgeConfig> {
  std::scoped_lock lock(runtime_mutex_);
  if (!config_) {
    config_ = load_bridge_config(context.config());
  }
  return config_;
}

auto BridgeActor::resolve_forwarder(obcx::core::ActorContext &context,
                                    boost::asio::any_io_executor executor)
    -> std::shared_ptr<IBridgeForwarder> {
  if (auto injected = context.get_service<IBridgeForwarder>()) {
    return injected;
  }
  std::scoped_lock lock(runtime_mutex_);
  if (forwarder_) {
    return forwarder_;
  }

  auto bot_registry = context.get_service<obcx::core::BotRegistry>();
  if (!bot_registry) {
    return nullptr;
  }

  auto db_manager = context.get_service<obcx::core::DbManager>();
  if (!db_manager) {
    throw std::runtime_error("bridge forwarding requires DbManager service");
  }
  auto blocking_executor = context.get_service<obcx::core::BlockingExecutor>();
  if (!blocking_executor) {
    throw std::runtime_error(
        "bridge forwarding requires BlockingExecutor service");
  }

  const auto db_instance = context.db_instance().empty()
                               ? std::string{"main"}
                               : context.db_instance();
  received_message_repository_ = std::make_shared<ReceivedMessageRepository>(
      *db_manager, db_instance, "message_store");

  forwarder_ = std::make_shared<BridgeForwardingRuntime>(
      std::move(bot_registry), resolve_config(context),
      resolve_repository(context), received_message_repository_,
      std::move(executor), std::move(blocking_executor));
  return forwarder_;
}

auto BridgeActor::handle(const commands::RecallCommand &request,
                         const obcx::core::MessageEnvelope &message,
                         obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  return handle_command(request.invocation, message, context);
}

auto BridgeActor::handle(const commands::CheckAliveCommand &request,
                         const obcx::core::MessageEnvelope &message,
                         obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  return handle_command(request.invocation, message, context);
}

auto BridgeActor::handle(const commands::PokeCommand &request,
                         const obcx::core::MessageEnvelope &message,
                         obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  return handle_command(request.invocation, message, context);
}

auto BridgeActor::handle_command(
    const obcx::command::CommandInvocation &invocation,
    const obcx::core::MessageEnvelope &message,
    obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  try {
    auto executor = context.get_service<asio::any_io_executor>();
    if (!executor) {
      co_return obcx::core::ActorResult::failed(
          "bridge_command_executor_missing",
          "bridge command handling requires an Asio executor", true);
    }
    auto forwarder =
        co_await context.run_blocking([this, &context, executor = *executor] {
          return resolve_forwarder(context, executor);
        });
    if (!forwarder) {
      co_return obcx::core::ActorResult::failed(
          "bridge_command_runtime_unavailable",
          "bridge command runtime is unavailable", true);
    }
    const auto handled = co_await context.await_asio(
        *executor, [forwarder, invocation]() -> asio::awaitable<bool> {
          co_return co_await forwarder->handle_command(invocation);
        });
    if (!handled) {
      co_return obcx::core::ActorResult::failed(
          "bridge_command_unsupported",
          "bridge command is unsupported for the source platform", false);
    }
    auto result = obcx::core::ActorResult::success();
    result.emit(
        obcx::command::CommandCompleted{
            .transaction_id = invocation.transaction_id,
            .propagation = obcx::command::Propagation::Continue,
        },
        message);
    co_return result;
  } catch (const std::exception &error) {
    co_return obcx::core::ActorResult::failed("bridge_command_failed",
                                              error.what(), true);
  }
}

auto BridgeActor::handle(
    const obcx::message_store::events::MessageStored &stored,
    const obcx::core::MessageEnvelope &message,
    obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  (void)stored;
  if (message.headers.contains(std::string{obcx::command::processed_header})) {
    co_return obcx::core::ActorResult::success();
  }
  try {
    auto executor = context.get_service<asio::any_io_executor>();
    if (!executor) {
      throw std::runtime_error(
          "bridge forwarding requires an Asio executor service");
    }
    auto [repository, forwarder] =
        co_await context.run_blocking([this, &context, executor = *executor] {
          return std::pair{resolve_repository(context),
                           resolve_forwarder(context, executor)};
        });
    if (forwarder) {
      auto forward_result = co_await context.await_asio(
          *executor,
          [forwarder, forwarded = message]() mutable
              -> asio::awaitable<BridgeForwardResult> {
            co_return co_await forwarder->forward_message(forwarded);
          });
      auto mapping = mapping_from_forward_result(forward_result);
      if (forward_result.disposition ==
          DirectForwardDisposition::NotForwarded) {
        auto result = obcx::core::ActorResult::failed(
            "bridge_not_forwarded",
            "bridge handler did not produce a direct delivery", true);
        result.emit(build_failed_event(
            message, "bridge_not_forwarded",
            "bridge handler did not produce a direct delivery", true));
        co_return result;
      }
      if (mapping.source_platform.empty() ||
          mapping.source_message_id.empty() ||
          mapping.target_platform.empty() ||
          mapping.target_message_id.empty()) {
        auto result = obcx::core::ActorResult::failed(
            "missing_forward_mapping",
            "forwarder must return source and target mapping fields", true);
        result.emit(build_failed_event(
            message, "missing_forward_mapping",
            "forwarder must return source and target mapping fields", true));
        co_return result;
      }

      if (forward_result.disposition == DirectForwardDisposition::NewDelivery) {
        bool persisted = false;
        try {
          persisted = co_await context.run_blocking([repository, mapping] {
            return repository->add_message_mapping(
                mapping, MessageMappingWritePurpose::DirectForward);
          });
        } catch (const std::exception &error) {
          auto result = obcx::core::ActorResult::failed(
              "mapping_persistence_failed", error.what(), false);
          result.emit(build_failed_event(message, "mapping_persistence_failed",
                                         error.what(), false));
          co_return result;
        }
        if (!persisted) {
          auto result = obcx::core::ActorResult::failed(
              "mapping_persistence_failed",
              "bridge mapping repository rejected the direct mapping", false);
          result.emit(build_failed_event(
              message, "mapping_persistence_failed",
              "bridge mapping repository rejected the direct mapping", false));
          co_return result;
        }
      }
      auto result = obcx::core::ActorResult::success();
      result.emit(
          build_forwarded_event(message, mapping, forward_result.target_bot));
      co_return result;
    }

    const auto target_platform =
        payload_string(message.payload, "target_platform");
    const auto target_message_id =
        payload_string(message.payload, "target_message_id");
    const auto source_id = source_message_id(message);
    if (target_platform.empty() || target_message_id.empty() ||
        source_id.empty()) {
      auto result = obcx::core::ActorResult::failed(
          "missing_forward_mapping",
          "MessageStored must include source and target mapping fields", false);
      result.emit(build_failed_event(
          message, "missing_forward_mapping",
          "MessageStored must include source and target mapping fields",
          false));
      co_return result;
    }

    storage::MessageMapping mapping{
        .source_platform = message.source_platform,
        .source_message_id = source_id,
        .target_platform = target_platform,
        .target_message_id = target_message_id,
        .created_at = std::chrono::system_clock::now(),
    };
    bool persisted = false;
    try {
      persisted = co_await context.run_blocking([repository, mapping] {
        return repository->add_message_mapping(
            mapping, MessageMappingWritePurpose::DirectForward);
      });
    } catch (const std::exception &error) {
      auto result = obcx::core::ActorResult::failed(
          "mapping_persistence_failed", error.what(), false);
      result.emit(build_failed_event(message, "mapping_persistence_failed",
                                     error.what(), false));
      co_return result;
    }
    if (!persisted) {
      auto result = obcx::core::ActorResult::failed(
          "mapping_persistence_failed",
          "bridge mapping repository rejected the direct mapping", false);
      result.emit(build_failed_event(
          message, "mapping_persistence_failed",
          "bridge mapping repository rejected the direct mapping", false));
      co_return result;
    }

    auto result = obcx::core::ActorResult::success();
    result.emit(build_forwarded_event(
        message, mapping, payload_string(message.payload, "target_bot")));
    co_return result;
  } catch (const BridgeRetryUnavailable &error) {
    auto result = obcx::core::ActorResult::failed("bridge_retry_unavailable",
                                                  error.what(), true);
    result.emit(build_failed_event(message, "bridge_retry_unavailable",
                                   error.what(), true));
    co_return result;
  } catch (const std::exception &error) {
    auto result =
        obcx::core::ActorResult::failed("bridge_error", error.what(), true);
    result.emit(
        build_failed_event(message, "bridge_error", error.what(), true));
    co_return result;
  }
}

auto BridgeActor::handle(const obcx::core::events::RawNoticeEvent &notice,
                         const obcx::core::MessageEnvelope &message,
                         obcx::core::ActorContext &context)
    -> obcx::core::ActorTask<obcx::core::ActorResult> {
  (void)notice;
  try {
    auto executor = context.get_service<asio::any_io_executor>();
    if (!executor) {
      co_return obcx::core::ActorResult::failed(
          "bridge_notice_executor_missing",
          "bridge notice handling requires an Asio executor", true);
    }
    auto forwarder =
        co_await context.run_blocking([this, &context, executor = *executor] {
          return resolve_forwarder(context, executor);
        });
    if (!forwarder) {
      co_return obcx::core::ActorResult::failed(
          "bridge_notice_runtime_unavailable",
          "bridge notice runtime is unavailable", true);
    }
    co_await context.await_asio(
        *executor,
        [forwarder, forwarded = message]() mutable -> asio::awaitable<void> {
          (void)co_await forwarder->handle_notice(forwarded);
        });
    co_return obcx::core::ActorResult::success();
  } catch (const std::exception &error) {
    co_return obcx::core::ActorResult::failed("bridge_notice_failed",
                                              error.what(), true);
  }
}

#ifndef OBCX_BRIDGE_ACTOR_NO_EXPORT
OBCX_ACTOR_EXPORT_V2(bridge::BridgeActor)
#endif

} // namespace bridge

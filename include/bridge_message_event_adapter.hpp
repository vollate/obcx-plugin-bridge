#pragma once

#include <common/message_type.hpp>
#include <core/actor_v2.hpp>

#include <optional>

namespace bridge {

auto message_event_from_message_stored(
    const obcx::core::MessageEnvelope &message)
    -> std::optional<obcx::common::MessageEvent>;

} // namespace bridge

#pragma once

#include <common/message_type.hpp>
#include <core/actor.hpp>

#include <optional>

namespace bridge {

auto message_event_from_message_stored(
    const obcx::core::MessageEnvelope &message)
    -> std::optional<obcx::common::MessageEvent>;

auto notice_event_from_raw_notice(const obcx::core::MessageEnvelope &message)
    -> std::optional<obcx::common::NoticeEvent>;

} // namespace bridge

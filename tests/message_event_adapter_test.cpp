#include "bridge_message_event_adapter.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

auto stored_message_with_raw() -> obcx::core::MessageEnvelope {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "stored-qq-raw";
  envelope.type = "obcx::message_store::events::MessageStored";
  envelope.source_platform = "qq";
  envelope.source_bot = "qq-main";
  envelope.timestamp = std::chrono::system_clock::time_point{
      std::chrono::milliseconds{1720000000123}};
  envelope.payload = {
      {"message_id", "qq-raw-1"},
      {"sender", "user-7"},
      {"group_id", "group-3"},
      {"message_type", "group"},
      {"payload", {{"text", "hello from raw"}}},
  };
  envelope.raw = {
      {"time", 1720000000.123},
      {"self_id", "qq-main"},
      {"post_type", "message"},
      {"message_type", "group"},
      {"sub_type", "normal"},
      {"message_id", 10001},
      {"user_id", 70007},
      {"group_id", 30003},
      {"raw_message", "hello from raw"},
      {"font", 0},
      {"message", {{{"type", "text"}, {"data", {{"text", "hello from raw"}}}}}},
  };
  return envelope;
}

} // namespace

TEST(BridgeMessageEventAdapterTest,
     RebuildsMessageEventFromMessageStoredRawPayload) {
  const auto event =
      bridge::message_event_from_message_stored(stored_message_with_raw());

  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->message_id, "10001");
  EXPECT_EQ(event->user_id, "70007");
  ASSERT_TRUE(event->group_id.has_value());
  EXPECT_EQ(event->group_id.value(), "30003");
  EXPECT_EQ(event->message_type, "group");
  EXPECT_EQ(event->sub_type, "normal");
  EXPECT_EQ(event->raw_message, "hello from raw");
  ASSERT_EQ(event->message.size(), 1);
  EXPECT_EQ(event->message.front().type, "text");
  EXPECT_EQ(event->message.front().data["text"], "hello from raw");
  EXPECT_EQ(event->data["message_id"], 10001);
}

TEST(BridgeMessageEventAdapterTest,
     RebuildsTextSegmentFromStoredPayloadWhenRawMessageIsSparse) {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "stored-tg-sparse";
  envelope.type = "obcx::message_store::events::MessageStored";
  envelope.source_platform = "telegram";
  envelope.source_bot = "tg-main";
  envelope.payload = {
      {"message_id", "tg-7"},
      {"sender", "user-9"},
      {"group_id", "chat-5"},
      {"message_type", "group"},
      {"payload", {{"text", "hello from payload"}}},
  };

  const auto event = bridge::message_event_from_message_stored(envelope);

  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->message_id, "tg-7");
  EXPECT_EQ(event->user_id, "user-9");
  ASSERT_TRUE(event->group_id.has_value());
  EXPECT_EQ(event->group_id.value(), "chat-5");
  EXPECT_EQ(event->message_type, "group");
  EXPECT_EQ(event->raw_message, "hello from payload");
  ASSERT_EQ(event->message.size(), 1);
  EXPECT_EQ(event->message.front().type, "text");
  EXPECT_EQ(event->message.front().data["text"], "hello from payload");
}

TEST(BridgeMessageEventAdapterTest, RebuildsPokeNoticeFromRawNoticeEnvelope) {
  obcx::core::MessageEnvelope envelope;
  envelope.id = "notice-qq-poke";
  envelope.type = "obcx::core::events::RawNoticeEvent";
  envelope.source_platform = "qq";
  envelope.source_bot = "qq-main";
  envelope.conversation_id = "group:qq-group";
  envelope.timestamp = std::chrono::system_clock::time_point{
      std::chrono::milliseconds{1720000000456}};
  envelope.payload = {
      {"notice_type", "notify"},
      {"sender", "user-7"},
      {"group_id", "qq-group"},
      {"payload", {{"sub_type", "poke"}, {"target_id", 80008}}},
  };
  envelope.raw = {
      {"time", 1720000000.456},  {"self_id", 90001},   {"post_type", "notice"},
      {"notice_type", "notify"}, {"sub_type", "poke"}, {"user_id", 70007},
      {"target_id", 80008},      {"group_id", 30003},
  };

  const auto event = bridge::notice_event_from_raw_notice(envelope);

  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->notice_type, "notify");
  EXPECT_EQ(event->user_id, "70007");
  ASSERT_TRUE(event->group_id.has_value());
  EXPECT_EQ(event->group_id.value(), "30003");
  EXPECT_EQ(event->data["sub_type"], "poke");
  EXPECT_EQ(event->data["target_id"], 80008);
}

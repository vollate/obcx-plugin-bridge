#include "qq/media_processor.hpp"
#include "qq/message_formatter.hpp"

#include "bridge_state_repository.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::qq {

auto QQMediaProcessor::process_at_segment(
    obcx::core::IBot &qq_bot, const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const GroupBridgeConfig *bridge_config)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment;
  converted_segment.type = "text";
  std::string qq_user_id = segment.data.value("qq", "unknown");
  converted_segment.data.clear();

  auto at_display_name =
      state_repository_ ? state_repository_->query_user_display_name(
                              "qq", qq_user_id, event.group_id.value_or(""))
                        : std::optional<std::string>{};

  if (!at_display_name.has_value()) {
    co_await QQMessageFormatter::fetch_and_save_user_info(
        state_repository_, qq_bot, qq_user_id, event.group_id.value());

    at_display_name =
        state_repository_ ? state_repository_->query_user_display_name(
                                "qq", qq_user_id, event.group_id.value_or(""))
                          : std::optional<std::string>{};
  }

  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    const TopicBridgeConfig *topic_config =
        bridge::get_topic_config(telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  if (show_sender && at_display_name.has_value()) {
    converted_segment.data["text"] =
        fmt::format("@{} ", at_display_name.value());
    PLUGIN_DEBUG("qq_to_tg", "转换QQ@消息: {} -> @{}", qq_user_id,
                 at_display_name.value());
  } else if (show_sender) {
    converted_segment.data["text"] = fmt::format("[@{}] ", qq_user_id);
  } else {
    // 不显示发送者：用空文本占位，避免在转发结果里残留 @用户ID。
    converted_segment.data["text"] = "";
    PLUGIN_DEBUG("qq_to_tg", "QQ@消息不显示发送者: {}", qq_user_id);
  }

  co_return converted_segment;
}

} // namespace bridge::qq

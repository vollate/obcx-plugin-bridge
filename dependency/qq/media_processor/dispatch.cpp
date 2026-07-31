#include "qq/media_processor.hpp"

#include <common/logger.hpp>

namespace bridge::qq {

QQMediaProcessor::QQMediaProcessor(
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)) {}

auto QQMediaProcessor::process_qq_media_segment(
    obcx::core::IBot &qq_bot, obcx::core::IBot &telegram_bot,
    const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>> {

  try {
    if (segment.type == "image") {
      co_return co_await process_image_segment(
          qq_bot, telegram_bot, segment, event, telegram_group_id, topic_id,
          sender_display_name, bridge_config, temp_files_to_cleanup);
    } else if (segment.type == "record") {
      co_return co_await process_record_segment(segment);
    } else if (segment.type == "video") {
      co_return co_await process_video_segment(segment);
    } else if (segment.type == "file") {
      co_return co_await process_file_segment(qq_bot, segment, event);
    } else if (segment.type == "face") {
      co_return co_await process_face_segment(segment);
    } else if (segment.type == "mface") {
      co_return co_await process_mface_segment(segment);
    } else if (segment.type == "at") {
      co_return co_await process_at_segment(
          qq_bot, segment, event, telegram_group_id, topic_id, bridge_config);
    } else if (segment.type == "shake") {
      co_return co_await process_shake_segment(segment);
    } else if (segment.type == "music") {
      co_return co_await process_music_segment(segment);
    } else if (segment.type == "share") {
      co_return co_await process_share_segment(segment);
    } else if (segment.type == "json") {
      co_return co_await process_json_segment(segment);
    } else if (segment.type == "app") {
      co_return co_await process_app_segment(segment);
    } else if (segment.type == "ark") {
      co_return co_await process_ark_segment(segment);
    } else if (segment.type == "miniapp") {
      co_return co_await process_miniapp_segment(segment);
    } else {
      co_return segment;
    }
  } catch (const std::exception &e) {
    OBCX_ERROR("转换QQ消息段失败: {}", e.what());
    co_return std::nullopt;
  }
}

} // namespace bridge::qq

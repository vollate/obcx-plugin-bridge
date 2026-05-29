// QQ媒体处理器：构造函数与消息段分发逻辑。
//
// 这里只负责按 segment.type 把工作分派到具体的处理函数。
// 每种 segment 的实际实现位于同一目录下的对应 .cpp 文件中。

#include "qq/media_processor.hpp"

#include <common/logger.hpp>

namespace bridge::qq {

QQMediaProcessor::QQMediaProcessor(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

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
      // 保持原样
      co_return segment;
    }
  } catch (const std::exception &e) {
    PLUGIN_ERROR("qq_to_tg", "转换QQ消息段失败: {}", e.what());
    co_return std::nullopt;
  }
}

} // namespace bridge::qq

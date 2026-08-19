#pragma once

#include "bridge_bot_operations.hpp"
#include "config.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bridge {
class BridgeStateRepository;
struct BridgeConfig;
} // namespace bridge

namespace bridge::qq {

struct MiniAppParseResult {
  bool success = false;
  std::string title;
  std::string description;
  std::vector<std::string> urls;
  std::string app_name;
  std::string raw_json;
};

class QQMediaProcessor {
public:
  explicit QQMediaProcessor(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const bridge::BridgeConfig> config,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor);

  auto process_qq_media_segment(const obcx::common::MessageSegment &segment,
                                const obcx::common::MessageEvent &event,
                                const std::string &telegram_group_id,
                                std::int64_t topic_id,
                                const std::string &sender_display_name,
                                const GroupBridgeConfig *bridge_config,
                                std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>>;
  auto process_image_segment(const obcx::common::MessageSegment &segment,
                             const obcx::common::MessageEvent &event,
                             const std::string &telegram_group_id,
                             std::int64_t topic_id,
                             const std::string &sender_display_name,
                             const GroupBridgeConfig *bridge_config,
                             std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>>;
  auto process_file_segment(const obcx::common::MessageSegment &segment,
                            const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_at_segment(const obcx::common::MessageSegment &segment,
                          const obcx::common::MessageEvent &event,
                          const std::string &telegram_group_id,
                          std::int64_t topic_id,
                          const GroupBridgeConfig *bridge_config)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  static auto process_record_segment(
      const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_video_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_face_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_mface_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_shake_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_music_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  static auto process_share_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_json_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_app_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_ark_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_miniapp_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

private:
  auto detect_gif_format(const std::string &url)
      -> boost::asio::awaitable<bool>;
  static auto is_sticker(const obcx::common::MessageSegment &segment) -> bool;
  auto handle_sticker_cache(const obcx::common::MessageSegment &segment,
                            const std::string &telegram_group_id,
                            std::int64_t topic_id,
                            const std::string &sender_display_name,
                            const GroupBridgeConfig *bridge_config)
      -> boost::asio::awaitable<bool>;
  auto parse_miniapp_json(const std::string &json_data) const
      -> MiniAppParseResult;
  auto format_miniapp_message(const MiniAppParseResult &parse_result) const
      -> obcx::common::MessageSegment;
  static auto extract_urls_from_json(const std::string &json_str)
      -> std::vector<std::string>;
  static auto to_hex_string(const std::string &data, size_t max_bytes = 16)
      -> std::string;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const bridge::BridgeConfig> config_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
};

} // namespace bridge::qq

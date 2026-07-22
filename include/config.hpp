#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace obcx::common {
class ActorConfigView;
}

namespace bridge {

enum class BridgeMode {
  GROUP_TO_GROUP,
  TOPIC_TO_GROUP,
};

struct TopicBridgeConfig {
  int64_t telegram_topic_id;
  std::string qq_group_id;
  bool show_qq_to_tg_sender;
  bool show_tg_to_qq_sender;
  bool enable_qq_to_tg;
  bool enable_tg_to_qq;

  TopicBridgeConfig(int64_t topic_id, std::string qq_id, bool qq_to_tg = true,
                    bool tg_to_qq = true, bool enable_qq_tg = true,
                    bool enable_tg_qq = true)
      : telegram_topic_id{topic_id}, qq_group_id{std::move(qq_id)},
        show_qq_to_tg_sender{qq_to_tg}, show_tg_to_qq_sender{tg_to_qq},
        enable_qq_to_tg{enable_qq_tg}, enable_tg_to_qq{enable_tg_qq} {}
};

struct GroupBridgeConfig {
  std::string telegram_group_id;
  BridgeMode mode = BridgeMode::GROUP_TO_GROUP;
  std::string qq_group_id;
  std::vector<TopicBridgeConfig> topics;
  bool show_qq_to_tg_sender = true;
  bool show_tg_to_qq_sender = true;
  bool enable_qq_to_tg = true;
  bool enable_tg_to_qq = true;

  GroupBridgeConfig() = default;

  GroupBridgeConfig(std::string tg_id, std::string qq_id, bool qq_to_tg = true,
                    bool tg_to_qq = true, bool enable_qq_tg = true,
                    bool enable_tg_qq = true)
      : telegram_group_id{std::move(tg_id)}, mode{BridgeMode::GROUP_TO_GROUP},
        qq_group_id{std::move(qq_id)}, show_qq_to_tg_sender{qq_to_tg},
        show_tg_to_qq_sender{tg_to_qq}, enable_qq_to_tg{enable_qq_tg},
        enable_tg_to_qq{enable_tg_qq} {}

  GroupBridgeConfig(std::string tg_id,
                    std::vector<TopicBridgeConfig> topic_configs,
                    bool qq_to_tg = true, bool tg_to_qq = true,
                    bool enable_qq_tg = true, bool enable_tg_qq = true)
      : telegram_group_id{std::move(tg_id)}, mode{BridgeMode::TOPIC_TO_GROUP},
        topics{std::move(topic_configs)}, show_qq_to_tg_sender{qq_to_tg},
        show_tg_to_qq_sender{tg_to_qq}, enable_qq_to_tg{enable_qq_tg},
        enable_tg_to_qq{enable_tg_qq} {}
};

/**
 * Immutable after construction and owned by one bridge actor generation.
 * Bot endpoints and credentials intentionally do not live here: they remain
 * process-owned through BotRegistry and are restart-required.
 */
struct BridgeConfig final {
  std::unordered_map<std::string, GroupBridgeConfig> group_map;

  bool enable_miniapp_parsing = true;
  bool show_raw_json_on_parse_fail = true;
  int max_json_display_length = 2000;

  bool enable_retry_queue = true;
  int message_retry_max_attempts = 5;
  int media_retry_max_attempts = 3;
  int message_retry_base_interval_sec = 2;
  int media_retry_base_interval_sec = 5;
  int retry_queue_check_interval_sec = 10;
  int max_retry_interval_sec = 300;

  std::string bridge_files_dir;
  std::string bridge_files_container_dir = "/root/llonebot/bridge_files";

  std::string ffmpeg_path = "ffmpeg";
  std::size_t gif_max_file_size = 0;
  int gif_max_duration = 5;
  int gif_max_fps = 0;
  int gif_max_width = 0;
  int gif_max_colors = 256;

  int image_url_probe_max_attempts = 3;
  int image_url_probe_base_delay_ms = 500;
  int image_url_probe_timeout_ms = 5000;
  std::string image_placeholder_url =
      "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";

  [[nodiscard]] auto qq_group_id_for_topic(std::string_view tg_group_id,
                                           int64_t topic_id) const
      -> std::string;
  [[nodiscard]] auto tg_group_and_topic_id(std::string_view qq_group_id) const
      -> std::pair<std::string, int64_t>;
  [[nodiscard]] auto bridge_config(std::string_view tg_group_id) const
      -> const GroupBridgeConfig *;
  [[nodiscard]] auto topic_config(std::string_view tg_group_id,
                                  int64_t topic_id) const
      -> const TopicBridgeConfig *;
};

[[nodiscard]] auto load_bridge_config(const obcx::common::ActorConfigView &view)
    -> std::shared_ptr<const BridgeConfig>;

/** Validate bridge-owned values after loading one immutable generation. */
void validate_bridge_config(const BridgeConfig &config);

/** Whether the supplied immutable snapshot assigns forwarding to actors. */
[[nodiscard]] auto actor_pipeline_enabled(
    const obcx::common::ActorConfigView &view) -> bool;

} // namespace bridge

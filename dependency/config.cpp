#include "config.hpp"

#include <common/config_loader.hpp>
#include <common/logger.hpp>

#include <stdexcept>
#include <utility>

namespace bridge {
namespace {

template <typename T>
void assign_if_present(const obcx::common::ActorConfigView &view,
                       std::string_view key, T &target) {
  if (auto value = view.get_value<T>(key)) {
    target = std::move(*value);
  }
}

void load_group_mappings(const obcx::common::ActorConfigView &view,
                         BridgeConfig &result) {
  const auto section = view.get_root_section("group_mappings");
  if (!section.has_value()) {
    OBCX_WARN("No group_mappings section found in generation config");
    return;
  }

  const auto &mappings = section.value();
  if (const auto *groups = mappings["group_to_group"].as_array()) {
    for (const auto &item : *groups) {
      const auto *table = item.as_table();
      if (table == nullptr) {
        continue;
      }
      auto telegram_group_id =
          (*table)["telegram_group_id"].value_or<std::string>("");
      auto qq_group_id = (*table)["qq_group_id"].value_or<std::string>("");
      if (telegram_group_id.empty() || qq_group_id.empty()) {
        continue;
      }
      result.group_map.insert_or_assign(
          telegram_group_id,
          GroupBridgeConfig{telegram_group_id, qq_group_id,
                            (*table)["show_qq_to_tg_sender"].value_or(true),
                            (*table)["show_tg_to_qq_sender"].value_or(true),
                            (*table)["enable_qq_to_tg"].value_or(true),
                            (*table)["enable_tg_to_qq"].value_or(true)});
    }
  }

  if (const auto *topic_groups = mappings["topic_to_group"].as_array()) {
    for (const auto &item : *topic_groups) {
      const auto *table = item.as_table();
      if (table == nullptr) {
        continue;
      }
      auto telegram_group_id =
          (*table)["telegram_group_id"].value_or<std::string>("");
      if (telegram_group_id.empty()) {
        continue;
      }

      std::vector<TopicBridgeConfig> topics;
      if (const auto *topic_entries = mappings["topics"].as_array()) {
        for (const auto &topic_item : *topic_entries) {
          const auto *topic = topic_item.as_table();
          if (topic == nullptr ||
              (*topic)["telegram_group_id"].value_or<std::string>("") !=
                  telegram_group_id) {
            continue;
          }
          const auto topic_id =
              (*topic)["telegram_topic_id"].value_or<int64_t>(-1);
          auto qq_group_id = (*topic)["qq_group_id"].value_or<std::string>("");
          if (topic_id == -1 || qq_group_id.empty()) {
            continue;
          }
          topics.emplace_back(topic_id, std::move(qq_group_id),
                              (*topic)["show_qq_to_tg_sender"].value_or(true),
                              (*topic)["show_tg_to_qq_sender"].value_or(true),
                              (*topic)["enable_qq_to_tg"].value_or(true),
                              (*topic)["enable_tg_to_qq"].value_or(true));
        }
      }

      if (!topics.empty()) {
        result.group_map.insert_or_assign(
            telegram_group_id,
            GroupBridgeConfig{telegram_group_id, std::move(topics),
                              (*table)["show_qq_to_tg_sender"].value_or(true),
                              (*table)["show_tg_to_qq_sender"].value_or(true),
                              (*table)["enable_qq_to_tg"].value_or(true),
                              (*table)["enable_tg_to_qq"].value_or(true)});
      }
    }
  }

  OBCX_INFO("Loaded {} bridge mappings for actor generation",
            result.group_map.size());
}

auto actor_is_enabled(const toml::table &actors, std::string_view actor)
    -> bool {
  const auto *table = actors[actor].as_table();
  return table != nullptr && (*table)["enabled"].value_or(true);
}

} // namespace

auto BridgeConfig::qq_group_id_for_topic(std::string_view tg_group_id,
                                         int64_t topic_id) const
    -> std::string {
  const auto *mapping = bridge_config(tg_group_id);
  if (mapping == nullptr) {
    return {};
  }
  if (mapping->mode == BridgeMode::GROUP_TO_GROUP) {
    return mapping->qq_group_id;
  }
  const auto *topic = topic_config(tg_group_id, topic_id);
  return topic == nullptr ? std::string{} : topic->qq_group_id;
}

auto BridgeConfig::tg_group_and_topic_id(std::string_view qq_group_id) const
    -> std::pair<std::string, int64_t> {
  for (const auto &[telegram_group_id, mapping] : group_map) {
    if (mapping.mode == BridgeMode::GROUP_TO_GROUP) {
      if (mapping.enable_qq_to_tg && mapping.qq_group_id == qq_group_id) {
        return {telegram_group_id, -1};
      }
      continue;
    }
    for (const auto &topic : mapping.topics) {
      if (topic.enable_qq_to_tg && topic.qq_group_id == qq_group_id) {
        return {telegram_group_id, topic.telegram_topic_id};
      }
    }
  }
  return {{}, -1};
}

auto BridgeConfig::bridge_config(std::string_view tg_group_id) const
    -> const GroupBridgeConfig * {
  const auto found = group_map.find(std::string{tg_group_id});
  return found == group_map.end() ? nullptr : &found->second;
}

auto BridgeConfig::topic_config(std::string_view tg_group_id,
                                int64_t topic_id) const
    -> const TopicBridgeConfig * {
  const auto *mapping = bridge_config(tg_group_id);
  if (mapping == nullptr || mapping->mode != BridgeMode::TOPIC_TO_GROUP) {
    return nullptr;
  }
  for (const auto &topic : mapping->topics) {
    if (topic.telegram_topic_id == topic_id) {
      return &topic;
    }
  }
  return nullptr;
}

auto load_bridge_config(const obcx::common::ActorConfigView &view)
    -> std::shared_ptr<const BridgeConfig> {
  if (!view.available()) {
    throw std::runtime_error("bridge requires a generation config service");
  }

  auto result = std::make_shared<BridgeConfig>();
  load_group_mappings(view, *result);

  assign_if_present(view, "enable_miniapp_parsing",
                    result->enable_miniapp_parsing);
  assign_if_present(view, "show_raw_json_on_parse_fail",
                    result->show_raw_json_on_parse_fail);
  assign_if_present(view, "max_json_display_length",
                    result->max_json_display_length);
  assign_if_present(view, "enable_retry_queue", result->enable_retry_queue);
  assign_if_present(view, "message_retry_max_attempts",
                    result->message_retry_max_attempts);
  assign_if_present(view, "media_retry_max_attempts",
                    result->media_retry_max_attempts);
  assign_if_present(view, "message_retry_base_interval_sec",
                    result->message_retry_base_interval_sec);
  assign_if_present(view, "media_retry_base_interval_sec",
                    result->media_retry_base_interval_sec);
  assign_if_present(view, "retry_queue_check_interval_sec",
                    result->retry_queue_check_interval_sec);
  assign_if_present(view, "max_retry_interval_sec",
                    result->max_retry_interval_sec);
  assign_if_present(view, "bridge_files_dir", result->bridge_files_dir);
  assign_if_present(view, "bridge_files_container_dir",
                    result->bridge_files_container_dir);
  assign_if_present(view, "ffmpeg_path", result->ffmpeg_path);
  assign_if_present(view, "gif_max_duration", result->gif_max_duration);
  assign_if_present(view, "gif_max_fps", result->gif_max_fps);
  assign_if_present(view, "gif_max_width", result->gif_max_width);
  assign_if_present(view, "gif_max_colors", result->gif_max_colors);
  assign_if_present(view, "image_url_probe_max_attempts",
                    result->image_url_probe_max_attempts);
  assign_if_present(view, "image_url_probe_base_delay_ms",
                    result->image_url_probe_base_delay_ms);
  assign_if_present(view, "image_url_probe_timeout_ms",
                    result->image_url_probe_timeout_ms);
  assign_if_present(view, "image_placeholder_url",
                    result->image_placeholder_url);

  if (auto value = view.get_value<int64_t>("gif_max_file_size")) {
    if (*value < 0) {
      throw std::runtime_error("bridge gif_max_file_size cannot be negative");
    }
    result->gif_max_file_size = static_cast<std::size_t>(*value);
  }
  if (result->bridge_files_dir.empty()) {
    throw std::runtime_error("bridge config requires bridge_files_dir");
  }
  if (result->ffmpeg_path.empty()) {
    result->ffmpeg_path = "ffmpeg";
  }

  validate_bridge_config(*result);

  return result;
}

void validate_bridge_config(const BridgeConfig &config) {
  if (config.message_retry_max_attempts <= 0) {
    throw std::runtime_error(
        "bridge message_retry_max_attempts must be positive");
  }
  if (config.message_retry_base_interval_sec <= 0) {
    throw std::runtime_error(
        "bridge message_retry_base_interval_sec must be positive");
  }
  if (config.retry_queue_check_interval_sec <= 0) {
    throw std::runtime_error(
        "bridge retry_queue_check_interval_sec must be positive");
  }
  if (config.max_retry_interval_sec <= 0) {
    throw std::runtime_error("bridge max_retry_interval_sec must be positive");
  }
  if (config.message_retry_base_interval_sec > config.max_retry_interval_sec) {
    throw std::runtime_error(
        "bridge message retry base interval cannot exceed the maximum");
  }
  if (config.retry_queue_check_interval_sec > config.max_retry_interval_sec) {
    throw std::runtime_error(
        "bridge retry queue check interval cannot exceed the maximum");
  }
}

auto actor_pipeline_enabled(const obcx::common::ActorConfigView &view) -> bool {
  const auto actors = view.get_root_section("actors");
  const auto pipelines = view.get_root_section("pipelines");
  if (!actors.has_value() || !pipelines.has_value() ||
      !actor_is_enabled(*actors, "message_store") ||
      !actor_is_enabled(*actors, "bridge")) {
    return false;
  }

  for (const auto &[_, pipeline_node] : *pipelines) {
    const auto *pipeline = pipeline_node.as_table();
    if (pipeline == nullptr ||
        (*pipeline)["source"].value_or<std::string>("") !=
            "obcx::core::events::RawMessageEvent") {
      continue;
    }
    const auto *stages = (*pipeline)["stages"].as_array();
    if (stages == nullptr) {
      continue;
    }

    bool persists = false;
    bool forwards = false;
    for (const auto &stage_node : *stages) {
      const auto *stage = stage_node.as_table();
      if (stage == nullptr) {
        continue;
      }
      const auto actor = (*stage)["actor"].value_or<std::string>("");
      const auto input = (*stage)["input"].value_or<std::string>("");
      persists = persists || (actor == "message_store" &&
                              input == "obcx::core::events::RawMessageEvent");
      forwards =
          forwards || (actor == "bridge" &&
                       input == "obcx::message_store::events::MessageStored");
    }
    if (persists && forwards) {
      return true;
    }
  }
  return false;
}

} // namespace bridge

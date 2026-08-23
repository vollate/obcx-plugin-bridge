#include "config.hpp"
#include "bridge_message_identity.hpp"

#include <common/config_loader.hpp>
#include <common/logger.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace bridge {
namespace {

constexpr std::size_t kMaxQqMediaDownloadBytes = 10U * 1024U * 1024U;
constexpr std::string_view kLegacyPairId = "legacy";

template <typename T>
void assign_if_present(const obcx::common::ActorConfigView &view,
                       std::string_view key, T &target) {
  if (auto value = view.get_value<T>(key)) {
    target = std::move(*value);
  }
}

auto mapping_pair_id(const toml::table &table, const BridgeConfig &config)
    -> std::string {
  auto pair_id = table["pair"].value_or<std::string>("");
  if (pair_id.empty()) {
    if (config.installation_pairs.size() != 1) {
      throw std::runtime_error(
          "bridge mapping pair is required when multiple installation pairs "
          "are configured");
    }
    pair_id = config.installation_pairs.begin()->first;
  }
  if (!config.installation_pairs.contains(pair_id)) {
    throw std::runtime_error("bridge mapping names unknown pair: " + pair_id);
  }
  return pair_id;
}

void add_group_mapping(BridgeInstallationPair &pair,
                       GroupBridgeConfig mapping) {
  const auto telegram_group_id = mapping.telegram_group_id;
  if (telegram_group_id.empty()) {
    throw std::runtime_error("bridge mapping requires telegram_group_id");
  }
  if (!pair.group_map.emplace(telegram_group_id, std::move(mapping)).second) {
    throw std::runtime_error(
        "bridge pair " + pair.id +
        " contains duplicate Telegram group route: " + telegram_group_id);
  }
}

void load_installation_pairs(const obcx::common::ActorConfigView &view,
                             BridgeConfig &result) {
  auto scalar_telegram =
      view.get_value<std::string>("telegram_installation").value_or("");
  auto scalar_onebot =
      view.get_value<std::string>("onebot11_installation").value_or("");
  const auto actor_config = view.get_section();
  const auto *named_pairs =
      actor_config ? (*actor_config)["installation_pairs"].as_array() : nullptr;

  if (named_pairs != nullptr &&
      (!scalar_telegram.empty() || !scalar_onebot.empty())) {
    throw std::runtime_error(
        "bridge scalar and named installation pair forms cannot be mixed");
  }

  if (named_pairs != nullptr) {
    if (named_pairs->empty()) {
      throw std::runtime_error(
          "bridge installation_pairs must contain at least one pair");
    }
    for (const auto &item : *named_pairs) {
      const auto *table = item.as_table();
      if (table == nullptr) {
        throw std::runtime_error(
            "bridge installation_pairs entries must be tables");
      }
      BridgeInstallationPair pair{
          .id = (*table)["id"].value_or<std::string>(""),
          .telegram_installation =
              (*table)["telegram_installation"].value_or<std::string>(""),
          .onebot11_installation =
              (*table)["onebot11_installation"].value_or<std::string>(""),
      };
      if (pair.id.empty() || pair.telegram_installation.empty() ||
          pair.onebot11_installation.empty()) {
        throw std::runtime_error(
            "bridge installation pair requires non-empty id, "
            "telegram_installation, and onebot11_installation");
      }
      const auto id = pair.id;
      if (!result.installation_pairs.emplace(id, std::move(pair)).second) {
        throw std::runtime_error("bridge contains duplicate pair id: " + id);
      }
    }
  } else {
    result.legacy_scalar_form = true;
    if (scalar_telegram.empty() || scalar_onebot.empty()) {
      throw std::runtime_error(
          "bridge config requires telegram_installation and "
          "onebot11_installation, or installation_pairs");
    }
    result.installation_pairs.emplace(
        std::string{kLegacyPairId},
        BridgeInstallationPair{
            .id = std::string{kLegacyPairId},
            .telegram_installation = std::move(scalar_telegram),
            .onebot11_installation = std::move(scalar_onebot),
        });
  }

  assign_if_present(view, "legacy_state_pair", result.legacy_state_pair);
}

auto configured_pair_id(const toml::table &table, const BridgeConfig &config,
                        const std::string_view field) -> std::string {
  auto pair_id = table[field].value_or<std::string>("");
  if (pair_id.empty()) {
    if (config.installation_pairs.size() != 1) {
      throw std::runtime_error(
          "bridge legacy mapping route requires pair in multi-pair mode");
    }
    pair_id = config.installation_pairs.begin()->first;
  }
  if (!config.installation_pairs.contains(pair_id)) {
    throw std::runtime_error(
        "bridge legacy mapping route names unknown pair: " + pair_id);
  }
  return pair_id;
}

auto configured_conversation(const toml::table &table,
                             const std::string_view conversation_key,
                             const std::string_view native_key,
                             const std::string_view platform) -> std::string {
  const auto conversation = table[conversation_key].value_or<std::string>("");
  const auto native = table[native_key].value_or<std::string>("");
  if (!conversation.empty() && !native.empty()) {
    throw std::runtime_error(
        "bridge legacy mapping route cannot mix conversation and group ids");
  }
  if (!conversation.empty()) {
    if (!valid_conversation_id(platform, conversation)) {
      throw std::runtime_error(
          "bridge legacy mapping route has invalid canonical conversation");
    }
    return conversation;
  }
  if (native.empty()) {
    throw std::runtime_error(
        "bridge legacy mapping route requires both conversations");
  }
  return canonical_conversation_id(platform, native);
}

void load_migration_configuration(const obcx::common::ActorConfigView &view,
                                  BridgeConfig &result) {
  const auto policy =
      view.get_value<std::string>("legacy_unresolved_mapping_policy")
          .value_or("fail");
  if (policy == "fail") {
    result.legacy_unresolved_mapping_policy =
        LegacyUnresolvedMappingPolicy::Fail;
  } else if (policy == "archive") {
    result.legacy_unresolved_mapping_policy =
        LegacyUnresolvedMappingPolicy::Archive;
  } else {
    throw std::runtime_error(
        "bridge legacy_unresolved_mapping_policy must be fail or archive");
  }

  const auto actor_config = view.get_section();
  const auto *routes = actor_config
                           ? (*actor_config)["legacy_mapping_routes"].as_array()
                           : nullptr;
  if (routes == nullptr) {
    return;
  }
  for (const auto &item : *routes) {
    const auto *table = item.as_table();
    if (table == nullptr) {
      throw std::runtime_error(
          "bridge legacy_mapping_routes entries must be tables");
    }
    const auto pair_id = configured_pair_id(*table, result, "pair");
    const auto &pair = result.installation_pairs.at(pair_id);
    const auto telegram_conversation = configured_conversation(
        *table, "telegram_conversation_id", "telegram_group_id", "telegram");
    const auto qq_conversation = configured_conversation(
        *table, "qq_conversation_id", "qq_group_id", "qq");
    const auto topic_id =
        (*table)["telegram_topic_id"].value_or<std::int64_t>(-1);
    if (topic_id == 0 || topic_id < -1) {
      throw std::runtime_error(
          "bridge legacy mapping route topic must be positive or -1");
    }
    result.legacy_mapping_routes.push_back(
        {.pair_id = pair_id,
         .telegram_installation = pair.telegram_installation,
         .onebot11_installation = pair.onebot11_installation,
         .telegram_conversation_id = telegram_conversation,
         .telegram_topic_id = topic_id,
         .qq_conversation_id = qq_conversation});
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
        throw std::runtime_error(
            "bridge group_to_group entries must be tables");
      }
      const auto pair_id = mapping_pair_id(*table, result);
      auto telegram_group_id =
          (*table)["telegram_group_id"].value_or<std::string>("");
      auto qq_group_id = (*table)["qq_group_id"].value_or<std::string>("");
      if (telegram_group_id.empty() || qq_group_id.empty()) {
        throw std::runtime_error(
            "bridge group_to_group mapping requires Telegram and QQ group "
            "ids");
      }
      add_group_mapping(
          result.installation_pairs.at(pair_id),
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
        throw std::runtime_error(
            "bridge topic_to_group entries must be tables");
      }
      const auto pair_id = mapping_pair_id(*table, result);
      auto telegram_group_id =
          (*table)["telegram_group_id"].value_or<std::string>("");
      if (telegram_group_id.empty()) {
        throw std::runtime_error(
            "bridge topic_to_group mapping requires telegram_group_id");
      }
      add_group_mapping(
          result.installation_pairs.at(pair_id),
          GroupBridgeConfig{telegram_group_id, std::vector<TopicBridgeConfig>{},
                            (*table)["show_qq_to_tg_sender"].value_or(true),
                            (*table)["show_tg_to_qq_sender"].value_or(true),
                            (*table)["enable_qq_to_tg"].value_or(true),
                            (*table)["enable_tg_to_qq"].value_or(true)});
    }
  }

  if (const auto *topics = mappings["topics"].as_array()) {
    for (const auto &item : *topics) {
      const auto *table = item.as_table();
      if (table == nullptr) {
        throw std::runtime_error("bridge topics entries must be tables");
      }
      const auto pair_id = mapping_pair_id(*table, result);
      auto telegram_group_id =
          (*table)["telegram_group_id"].value_or<std::string>("");
      const auto topic_id =
          (*table)["telegram_topic_id"].value_or<std::int64_t>(-1);
      auto qq_group_id = (*table)["qq_group_id"].value_or<std::string>("");
      if (telegram_group_id.empty() || topic_id <= 0 || qq_group_id.empty()) {
        throw std::runtime_error(
            "bridge topic mapping requires Telegram group, positive topic, "
            "and QQ group ids");
      }
      auto &pair = result.installation_pairs.at(pair_id);
      const auto route = pair.group_map.find(telegram_group_id);
      if (route == pair.group_map.end() ||
          route->second.mode != BridgeMode::TOPIC_TO_GROUP) {
        throw std::runtime_error("bridge topic mapping has no topic_to_group "
                                 "parent in pair " +
                                 pair_id);
      }
      if (std::ranges::any_of(route->second.topics, [&](const auto &topic) {
            return topic.telegram_topic_id == topic_id;
          })) {
        throw std::runtime_error("bridge pair " + pair_id +
                                 " contains duplicate Telegram topic route");
      }
      route->second.topics.emplace_back(
          topic_id, std::move(qq_group_id),
          (*table)["show_qq_to_tg_sender"].value_or(true),
          (*table)["show_tg_to_qq_sender"].value_or(true),
          (*table)["enable_qq_to_tg"].value_or(true),
          (*table)["enable_tg_to_qq"].value_or(true));
    }
  }

  std::size_t route_count = 0;
  for (const auto &[_, pair] : result.installation_pairs) {
    route_count += pair.group_map.size();
  }
  OBCX_DEBUG("Bridge actor mapping cache initialized: pairs={}, routes={}",
             result.installation_pairs.size(), route_count);
}

auto actor_is_enabled(const toml::table &actors, std::string_view actor)
    -> bool {
  const auto *table = actors[actor].as_table();
  return table != nullptr && (*table)["enabled"].value_or(true);
}

void validate_installation(const toml::table &bots,
                           const std::string &installation,
                           const std::string_view expected_type,
                           const std::string &path) {
  const auto *configured = bots[installation].as_table();
  if (configured == nullptr) {
    throw std::runtime_error(
        path + " does not name a configured bot: " + installation);
  }
  if (!(*configured)["enabled"].value_or(true)) {
    throw std::runtime_error(path + " names a disabled bot: " + installation);
  }
  const auto actual_type = (*configured)["type"].value_or<std::string>("");
  if (actual_type != expected_type) {
    throw std::runtime_error(path + " requires " + std::string{expected_type} +
                             " but " + installation + " has type " +
                             actual_type);
  }
}

void validate_installation_pairs(const obcx::common::ActorConfigView &view,
                                 const BridgeConfig &config) {
  const auto bots = view.get_root_section("bots");
  if (!bots.has_value()) {
    throw std::runtime_error(
        "bridge installation pairs require the root bots section");
  }
  for (const auto &[pair_id, pair] : config.installation_pairs) {
    validate_installation(*bots, pair.telegram_installation, "telegram",
                          "bridge pair " + pair_id + " telegram_installation");
    validate_installation(*bots, pair.onebot11_installation, "qq",
                          "bridge pair " + pair_id + " onebot11_installation");
  }
}

void validate_pair_routes(const BridgeInstallationPair &pair) {
  std::unordered_set<std::string> qq_source_routes;
  for (const auto &[_, mapping] : pair.group_map) {
    if (mapping.mode == BridgeMode::GROUP_TO_GROUP) {
      if (mapping.enable_qq_to_tg &&
          !qq_source_routes.insert(mapping.qq_group_id).second) {
        throw std::runtime_error(
            "bridge pair " + pair.id +
            " contains a fan-out QQ source route: " + mapping.qq_group_id);
      }
      continue;
    }
    if (mapping.topics.empty()) {
      throw std::runtime_error("bridge pair " + pair.id +
                               " contains a topic route without topics");
    }
    for (const auto &topic : mapping.topics) {
      if (topic.enable_qq_to_tg &&
          !qq_source_routes.insert(topic.qq_group_id).second) {
        throw std::runtime_error(
            "bridge pair " + pair.id +
            " contains a fan-out QQ source route: " + topic.qq_group_id);
      }
    }
  }
}

} // namespace

auto BridgeInstallationPair::qq_group_id_for_topic(
    const std::string_view tg_group_id, const int64_t topic_id) const
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

auto BridgeInstallationPair::tg_group_and_topic_id(
    const std::string_view qq_group_id) const
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

auto BridgeInstallationPair::bridge_config(
    const std::string_view tg_group_id) const -> const GroupBridgeConfig * {
  const auto found = group_map.find(std::string{tg_group_id});
  return found == group_map.end() ? nullptr : &found->second;
}

auto BridgeInstallationPair::topic_config(const std::string_view tg_group_id,
                                          const int64_t topic_id) const
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

auto BridgeConfig::pair(const std::string_view pair_id) const
    -> const BridgeInstallationPair * {
  const auto found = installation_pairs.find(std::string{pair_id});
  return found == installation_pairs.end() ? nullptr : &found->second;
}

auto BridgeConfig::pair_for_source(const std::string_view source_platform,
                                   const std::string_view source_installation)
    const -> const BridgeInstallationPair * {
  for (const auto &[_, candidate] : installation_pairs) {
    if ((source_platform == "telegram" &&
         candidate.telegram_installation == source_installation) ||
        (source_platform == "qq" &&
         candidate.onebot11_installation == source_installation)) {
      return &candidate;
    }
  }
  return nullptr;
}

auto BridgeConfig::legacy_migration_pair() const
    -> const BridgeInstallationPair * {
  if (legacy_scalar_form || installation_pairs.size() == 1) {
    return installation_pairs.empty() ? nullptr
                                      : &installation_pairs.begin()->second;
  }
  return legacy_state_pair.empty() ? nullptr : pair(legacy_state_pair);
}

auto BridgeConfig::migration_context(const bool allow_migration) const
    -> BridgeStateMigrationContext {
  BridgeStateMigrationContext context;
  if (const auto *legacy = legacy_migration_pair()) {
    context.pair_id = legacy->id;
    context.telegram_installation = legacy->telegram_installation;
    context.onebot11_installation = legacy->onebot11_installation;
  }
  context.unresolved_mapping_policy = legacy_unresolved_mapping_policy;
  context.allow_legacy_migration = allow_migration;

  const auto append_route = [&](LegacyConversationRoute route) {
    const auto duplicate = std::ranges::find_if(
        context.conversation_routes, [&](const auto &existing) {
          return existing.telegram_installation ==
                     route.telegram_installation &&
                 existing.onebot11_installation ==
                     route.onebot11_installation &&
                 existing.telegram_conversation_id ==
                     route.telegram_conversation_id &&
                 existing.telegram_topic_id == route.telegram_topic_id &&
                 existing.qq_conversation_id == route.qq_conversation_id;
        });
    if (duplicate == context.conversation_routes.end()) {
      context.conversation_routes.push_back(std::move(route));
    }
  };

  for (const auto &[pair_id, installation_pair] : installation_pairs) {
    for (const auto &[telegram_group_id, mapping] :
         installation_pair.group_map) {
      if (mapping.mode == BridgeMode::GROUP_TO_GROUP) {
        append_route(
            {.pair_id = pair_id,
             .telegram_installation = installation_pair.telegram_installation,
             .onebot11_installation = installation_pair.onebot11_installation,
             .telegram_conversation_id =
                 telegram_conversation_id(telegram_group_id),
             .telegram_topic_id = -1,
             .qq_conversation_id = qq_conversation_id(mapping.qq_group_id)});
        continue;
      }
      for (const auto &topic : mapping.topics) {
        append_route(
            {.pair_id = pair_id,
             .telegram_installation = installation_pair.telegram_installation,
             .onebot11_installation = installation_pair.onebot11_installation,
             .telegram_conversation_id =
                 telegram_conversation_id(telegram_group_id),
             .telegram_topic_id = topic.telegram_topic_id,
             .qq_conversation_id = qq_conversation_id(topic.qq_group_id)});
      }
    }
  }
  for (const auto &route : legacy_mapping_routes) {
    append_route(route);
  }
  return context;
}

auto BridgeConfig::qq_group_id_for_topic(const std::string_view pair_id,
                                         const std::string_view tg_group_id,
                                         const int64_t topic_id) const
    -> std::string {
  const auto *mapping = bridge_config(pair_id, tg_group_id);
  if (mapping == nullptr) {
    return {};
  }
  if (mapping->mode == BridgeMode::GROUP_TO_GROUP) {
    return mapping->qq_group_id;
  }
  const auto *topic = topic_config(pair_id, tg_group_id, topic_id);
  return topic == nullptr ? std::string{} : topic->qq_group_id;
}

auto BridgeConfig::tg_group_and_topic_id(
    const std::string_view pair_id, const std::string_view qq_group_id) const
    -> std::pair<std::string, int64_t> {
  const auto *selected = pair(pair_id);
  if (selected != nullptr) {
    return selected->tg_group_and_topic_id(qq_group_id);
  }
  if (pair_id != kLegacyPairId) {
    return {{}, -1};
  }
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

auto BridgeConfig::bridge_config(const std::string_view pair_id,
                                 const std::string_view tg_group_id) const
    -> const GroupBridgeConfig * {
  const auto *selected = pair(pair_id);
  if (selected != nullptr) {
    return selected->bridge_config(tg_group_id);
  }
  if (pair_id != kLegacyPairId) {
    return nullptr;
  }
  const auto found = group_map.find(std::string{tg_group_id});
  return found == group_map.end() ? nullptr : &found->second;
}

auto BridgeConfig::topic_config(const std::string_view pair_id,
                                const std::string_view tg_group_id,
                                const int64_t topic_id) const
    -> const TopicBridgeConfig * {
  const auto *mapping = bridge_config(pair_id, tg_group_id);
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
  load_installation_pairs(view, *result);
  load_group_mappings(view, *result);
  load_migration_configuration(view, *result);

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

  if (auto value = view.get_value<int64_t>("qq_media_download_max_bytes")) {
    if (*value <= 0) {
      throw std::runtime_error(
          "bridge qq_media_download_max_bytes must be positive");
    }
    result->qq_media_download_max_bytes = static_cast<std::size_t>(*value);
  }
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
  validate_installation_pairs(view, *result);

  if (result->installation_pairs.size() == 1) {
    const auto &only = result->installation_pairs.begin()->second;
    result->telegram_installation = only.telegram_installation;
    result->onebot11_installation = only.onebot11_installation;
    result->group_map = only.group_map;
  }
  return result;
}

void validate_bridge_config(const BridgeConfig &config) {
  if (config.installation_pairs.empty()) {
    throw std::runtime_error(
        "bridge config requires at least one installation pair");
  }
  std::unordered_set<std::string> telegram_installations;
  std::unordered_set<std::string> onebot_installations;
  for (const auto &[pair_id, pair] : config.installation_pairs) {
    if (pair_id.empty() || pair.id != pair_id ||
        pair.telegram_installation.empty() ||
        pair.onebot11_installation.empty()) {
      throw std::runtime_error("bridge contains an invalid installation pair");
    }
    if (pair.telegram_installation == pair.onebot11_installation) {
      throw std::runtime_error("bridge pair " + pair_id +
                               " must use different Telegram and OneBot "
                               "installations");
    }
    if (!telegram_installations.insert(pair.telegram_installation).second) {
      throw std::runtime_error(
          "bridge Telegram installation belongs to more than one pair: " +
          pair.telegram_installation);
    }
    if (!onebot_installations.insert(pair.onebot11_installation).second) {
      throw std::runtime_error(
          "bridge OneBot installation belongs to more than one pair: " +
          pair.onebot11_installation);
    }
    validate_pair_routes(pair);
  }
  if (!config.legacy_state_pair.empty() &&
      !config.installation_pairs.contains(config.legacy_state_pair)) {
    throw std::runtime_error("bridge legacy_state_pair names unknown pair: " +
                             config.legacy_state_pair);
  }

  std::unordered_map<std::string, std::string> current_qq_routes;
  std::unordered_map<std::string, std::string> current_telegram_routes;
  for (const auto &[pair_id, pair] : config.installation_pairs) {
    for (const auto &[telegram_group, mapping] : pair.group_map) {
      if (mapping.mode == BridgeMode::GROUP_TO_GROUP) {
        current_qq_routes.emplace(pair_id + "|" +
                                      qq_conversation_id(mapping.qq_group_id),
                                  telegram_conversation_id(telegram_group));
        current_telegram_routes.emplace(
            pair_id + "|" + telegram_conversation_id(telegram_group) + "|-1",
            qq_conversation_id(mapping.qq_group_id));
        continue;
      }
      for (const auto &topic : mapping.topics) {
        current_qq_routes.emplace(pair_id + "|" +
                                      qq_conversation_id(topic.qq_group_id),
                                  telegram_conversation_id(telegram_group));
        current_telegram_routes.emplace(
            pair_id + "|" + telegram_conversation_id(telegram_group) + "|" +
                std::to_string(topic.telegram_topic_id),
            qq_conversation_id(topic.qq_group_id));
      }
    }
  }

  std::unordered_set<std::string> legacy_qq_routes;
  std::unordered_set<std::string> legacy_telegram_routes;
  for (const auto &route : config.legacy_mapping_routes) {
    const auto *selected = config.pair(route.pair_id);
    if (selected == nullptr ||
        selected->telegram_installation != route.telegram_installation ||
        selected->onebot11_installation != route.onebot11_installation) {
      throw std::runtime_error(
          "bridge legacy mapping route has inconsistent pair ownership");
    }
    if (!valid_conversation_id("telegram", route.telegram_conversation_id) ||
        !valid_conversation_id("qq", route.qq_conversation_id) ||
        route.telegram_topic_id == 0 || route.telegram_topic_id < -1) {
      throw std::runtime_error(
          "bridge legacy mapping route has invalid conversation scope");
    }
    const auto qq_key = route.pair_id + "|" + route.qq_conversation_id;
    const auto telegram_key = route.pair_id + "|" +
                              route.telegram_conversation_id + "|" +
                              std::to_string(route.telegram_topic_id);
    if (!legacy_qq_routes.insert(qq_key).second ||
        !legacy_telegram_routes.insert(telegram_key).second) {
      throw std::runtime_error(
          "bridge contains duplicate legacy mapping route");
    }
    if (current_qq_routes.contains(qq_key) ||
        current_telegram_routes.contains(telegram_key)) {
      throw std::runtime_error(
          "bridge legacy mapping route conflicts with a current route");
    }
  }
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
  if (config.qq_media_download_max_bytes == 0 ||
      config.qq_media_download_max_bytes > kMaxQqMediaDownloadBytes) {
    throw std::runtime_error(
        "bridge qq_media_download_max_bytes must be between 1 and 10485760");
  }
}

auto resolve_bridge_source_pair(const BridgeConfig &config,
                                const std::string_view source_platform,
                                const std::string_view source_installation)
    -> const BridgeInstallationPair & {
  if (source_installation.empty()) {
    throw std::runtime_error(
        "bridge source_bot is required for exact installation routing");
  }
  if (source_platform != "telegram" && source_platform != "qq") {
    throw std::runtime_error("unsupported bridge source platform " +
                             std::string{source_platform});
  }
  const auto *selected =
      config.pair_for_source(source_platform, source_installation);
  if (selected == nullptr) {
    throw std::runtime_error(
        "bridge source_bot does not belong to a configured installation pair "
        "for " +
        std::string{source_platform});
  }
  return *selected;
}

void validate_bridge_source(const BridgeConfig &config,
                            const std::string_view source_platform,
                            const std::string_view source_installation) {
  (void)resolve_bridge_source_pair(config, source_platform,
                                   source_installation);
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

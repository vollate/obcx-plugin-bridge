#include "config.hpp"
#include "common/config_loader.hpp"
#include "common/logger.hpp"

#include <algorithm>

namespace bridge {

std::unordered_map<std::string, GroupBridgeConfig> GROUP_MAP;

void load_group_mappings() {
  try {
    GROUP_MAP.clear();

    auto config_section =
        obcx::common::ConfigLoader::instance().get_section("group_mappings");
    if (!config_section.has_value()) {
      OBCX_WARN("No group_mappings section found in config");
      return;
    }
    const auto &config = config_section.value();

    if (config.contains("group_to_group")) {
      const auto &group_to_group = config["group_to_group"];

      if (group_to_group.is_array()) {
        for (const auto &item : *group_to_group.as_array()) {
          if (!item.is_table())
            continue;
          const auto &item_table = *item.as_table();

          std::string telegram_group_id =
              item_table["telegram_group_id"].value_or<std::string>("");
          std::string qq_group_id =
              item_table["qq_group_id"].value_or<std::string>("");
          bool show_qq_to_tg_sender =
              item_table["show_qq_to_tg_sender"].value_or(true);
          bool show_tg_to_qq_sender =
              item_table["show_tg_to_qq_sender"].value_or(true);
          bool enable_qq_to_tg = item_table["enable_qq_to_tg"].value_or(true);
          bool enable_tg_to_qq = item_table["enable_tg_to_qq"].value_or(true);

          if (!telegram_group_id.empty() && !qq_group_id.empty()) {
            GroupBridgeConfig config(telegram_group_id, qq_group_id,
                                     show_qq_to_tg_sender, show_tg_to_qq_sender,
                                     enable_qq_to_tg, enable_tg_to_qq);
            GROUP_MAP[telegram_group_id] = config;
            OBCX_INFO("Loaded group mapping: {} -> {}",
                        telegram_group_id, qq_group_id);
          }
        }
      }
    }

    if (config.contains("topic_to_group")) {
      const auto &topic_to_group = config["topic_to_group"];

      if (topic_to_group.is_array()) {
        for (const auto &item : *topic_to_group.as_array()) {
          if (!item.is_table())
            continue;
          const auto &item_table = *item.as_table();

          std::string telegram_group_id =
              item_table["telegram_group_id"].value_or<std::string>("");
          bool show_qq_to_tg_sender =
              item_table["show_qq_to_tg_sender"].value_or(true);
          bool show_tg_to_qq_sender =
              item_table["show_tg_to_qq_sender"].value_or(true);
          bool enable_qq_to_tg = item_table["enable_qq_to_tg"].value_or(true);
          bool enable_tg_to_qq = item_table["enable_tg_to_qq"].value_or(true);

          if (!telegram_group_id.empty()) {
            std::vector<TopicBridgeConfig> topics;

            // topics live in a flat global array; filter by telegram_group_id
            if (config.contains("topics")) {
              const auto &topics_array = config["topics"];

              if (topics_array.is_array()) {
                for (const auto &topic_item : *topics_array.as_array()) {
                  if (!topic_item.is_table())
                    continue;
                  const auto &topic_table = *topic_item.as_table();

                  std::string topic_telegram_group_id =
                      topic_table["telegram_group_id"].value_or<std::string>(
                          "");

                  if (topic_telegram_group_id != telegram_group_id) {
                    continue;
                  }

                  int64_t telegram_topic_id =
                      topic_table["telegram_topic_id"].value_or<int64_t>(-1);
                  std::string qq_group_id =
                      topic_table["qq_group_id"].value_or<std::string>("");
                  bool topic_show_qq_to_tg =
                      topic_table["show_qq_to_tg_sender"].value_or(true);
                  bool topic_show_tg_to_qq =
                      topic_table["show_tg_to_qq_sender"].value_or(true);
                  bool topic_enable_qq_to_tg =
                      topic_table["enable_qq_to_tg"].value_or(true);
                  bool topic_enable_tg_to_qq =
                      topic_table["enable_tg_to_qq"].value_or(true);

                  if (telegram_topic_id != -1 && !qq_group_id.empty()) {
                    topics.emplace_back(
                        telegram_topic_id, qq_group_id, topic_show_qq_to_tg,
                        topic_show_tg_to_qq, topic_enable_qq_to_tg,
                        topic_enable_tg_to_qq);
                    OBCX_INFO("Loaded topic mapping: {}:{} -> {}",
                                telegram_group_id, telegram_topic_id,
                                qq_group_id);
                  }
                }
              }
            }

            if (!topics.empty()) {
              GroupBridgeConfig config(
                  telegram_group_id, topics, show_qq_to_tg_sender,
                  show_tg_to_qq_sender, enable_qq_to_tg, enable_tg_to_qq);
              GROUP_MAP[telegram_group_id] = config;
              OBCX_INFO("Loaded topic group mapping for TG {} with {} topics",
                          telegram_group_id, topics.size());
            }
          }
        }
      }
    }

    OBCX_INFO("Group mappings loaded: {} total mappings",
                GROUP_MAP.size());
  } catch (const std::exception &e) {
    OBCX_ERROR("Failed to load group mappings: {}", e.what());
  }
}

void initialize_config() {
  config::load_config();
  load_group_mappings();
}

bool actor_pipeline_enabled() {
  const auto &loader = obcx::common::ConfigLoader::instance();
  const auto actors = loader.get_actor_configs();
  const auto pipelines = loader.get_pipeline_configs();

  bool message_store_enabled = false;
  bool bridge_enabled = false;
  for (const auto &actor : actors) {
    if (actor.name == "message_store" && actor.enabled) {
      message_store_enabled = true;
    }
    if (actor.name == "bridge" && actor.enabled) {
      bridge_enabled = true;
    }
  }
  if (!message_store_enabled || !bridge_enabled) {
    return false;
  }

  for (const auto &pipeline : pipelines) {
    if (pipeline.source != "RawMessageEvent") {
      continue;
    }

    bool persists = false;
    bool forwards = false;
    for (const auto &stage : pipeline.stages) {
      if (stage.actor == "message_store" && stage.input == "RawMessageEvent" &&
          std::find(stage.outputs.begin(), stage.outputs.end(),
                    "MessageStored") != stage.outputs.end()) {
        persists = true;
      }
      if (stage.actor == "bridge" && stage.input == "MessageStored") {
        forwards = true;
      }
    }

    if (persists && forwards) {
      return true;
    }
  }

  return false;
}

namespace config {

std::string TELEGRAM_BOT_TOKEN;

std::string QQ_HOST;
uint16_t QQ_PORT;
std::string QQ_ACCESS_TOKEN;

std::string TELEGRAM_HOST;
uint16_t TELEGRAM_PORT;

std::string PROXY_HOST;
uint16_t PROXY_PORT;
std::string PROXY_TYPE;

std::string DATABASE_FILE;

bool ENABLE_MINIAPP_PARSING;
bool SHOW_RAW_JSON_ON_PARSE_FAIL;
int MAX_JSON_DISPLAY_LENGTH;

bool ENABLE_RETRY_QUEUE;
int MESSAGE_RETRY_MAX_ATTEMPTS;
int MEDIA_RETRY_MAX_ATTEMPTS;
int MESSAGE_RETRY_BASE_INTERVAL_SEC;
int MEDIA_RETRY_BASE_INTERVAL_SEC;
int RETRY_QUEUE_CHECK_INTERVAL_SEC;
int MAX_RETRY_INTERVAL_SEC;

std::string BRIDGE_FILES_DIR;
std::string BRIDGE_FILES_CONTAINER_DIR;

size_t GIF_MAX_FILE_SIZE = 0; // 0 = unlimited, preserve old lossless default
int GIF_MAX_DURATION = 5;
int GIF_MAX_FPS = 0;
int GIF_MAX_WIDTH = 0;
int GIF_MAX_COLORS = 256;

int IMAGE_URL_PROBE_MAX_ATTEMPTS = 3;
int IMAGE_URL_PROBE_BASE_DELAY_MS = 500;
int IMAGE_URL_PROBE_TIMEOUT_MS = 5000;
std::string IMAGE_PLACEHOLDER_URL =
    "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";

void load_config() {
  try {
    auto &loader = obcx::common::ConfigLoader::instance();
    OBCX_DEBUG("Config file path: {}", loader.get_config_path());
    OBCX_DEBUG("Config loaded: {}", loader.is_loaded());

    if (auto telegram_bot =
            loader.get_section("bots.telegram_bot.connection")) {
      TELEGRAM_BOT_TOKEN =
          telegram_bot->get("access_token")->value_or<std::string>("");
      TELEGRAM_HOST =
          telegram_bot->get("host")->value_or<std::string>("api.telegram.org");
      TELEGRAM_PORT = telegram_bot->get("port")->value_or<uint16_t>(443);

      PROXY_HOST =
          telegram_bot->get("proxy_host")->value_or<std::string>("127.0.0.1");
      PROXY_PORT = telegram_bot->get("proxy_port")->value_or<uint16_t>(20122);
      PROXY_TYPE =
          telegram_bot->get("proxy_type")->value_or<std::string>("http");
    }

    if (auto qq_bot = loader.get_section("bots.qq_bot.connection")) {
      QQ_HOST = qq_bot->get("host")->value_or<std::string>("127.0.0.1");
      QQ_PORT = qq_bot->get("port")->value_or<uint16_t>(3001);
      QQ_ACCESS_TOKEN = qq_bot->get("access_token")->value_or<std::string>("");
    }

    ENABLE_MINIAPP_PARSING = true;
    SHOW_RAW_JSON_ON_PARSE_FAIL = true;
    MAX_JSON_DISPLAY_LENGTH = 2000;
    ENABLE_RETRY_QUEUE = true;
    MESSAGE_RETRY_MAX_ATTEMPTS = 5;
    MEDIA_RETRY_MAX_ATTEMPTS = 3;
    MESSAGE_RETRY_BASE_INTERVAL_SEC = 2;
    MEDIA_RETRY_BASE_INTERVAL_SEC = 5;
    RETRY_QUEUE_CHECK_INTERVAL_SEC = 10;
    MAX_RETRY_INTERVAL_SEC = 300;

    constexpr std::string_view config_prefix = "actors.bridge.config.";
    const auto config_key = [config_prefix](const std::string_view field) {
      return std::string{config_prefix} + std::string{field};
    };

    if (auto database =
            loader.get_value<std::string>(config_key("database_file"))) {
      DATABASE_FILE = *database;
    }
    if (auto retry =
            loader.get_value<bool>(config_key("enable_retry_queue"))) {
      ENABLE_RETRY_QUEUE = *retry;
    }
    if (auto dir =
            loader.get_value<std::string>(config_key("bridge_files_dir"))) {
      BRIDGE_FILES_DIR = *dir;
      OBCX_INFO("Loaded bridge_files_dir: {}", BRIDGE_FILES_DIR);
    } else {
      throw std::runtime_error(
          "actors.bridge.config.bridge_files_dir must be configured");
    }
    if (auto dir = loader.get_value<std::string>(
            config_key("bridge_files_container_dir"))) {
      BRIDGE_FILES_CONTAINER_DIR = *dir;
    }

    if (auto val =
            loader.get_value<int64_t>(config_key("gif_max_file_size"))) {
      GIF_MAX_FILE_SIZE = static_cast<size_t>(*val);
    }
    if (auto val = loader.get_value<int64_t>(config_key("gif_max_duration"))) {
      GIF_MAX_DURATION = static_cast<int>(*val);
    }
    if (auto val = loader.get_value<int64_t>(config_key("gif_max_fps"))) {
      GIF_MAX_FPS = static_cast<int>(*val);
    }
    if (auto val = loader.get_value<int64_t>(config_key("gif_max_width"))) {
      GIF_MAX_WIDTH = static_cast<int>(*val);
    }
    if (auto val = loader.get_value<int64_t>(config_key("gif_max_colors"))) {
      GIF_MAX_COLORS = static_cast<int>(*val);
    }

    OBCX_INFO("Configuration loaded successfully");
    OBCX_INFO("Telegram Bot Token: {}...",
                TELEGRAM_BOT_TOKEN.substr(0, 20));
    OBCX_INFO("QQ Host: {}:{}", QQ_HOST, QQ_PORT);
    OBCX_INFO("Database: {}", DATABASE_FILE);
    OBCX_INFO("Bridge files dir (host): {}", BRIDGE_FILES_DIR);
    OBCX_INFO("Bridge files dir (container): {}",
                BRIDGE_FILES_CONTAINER_DIR);

  } catch (const std::exception &e) {
    OBCX_ERROR("Failed to load configuration: {}", e.what());
    TELEGRAM_BOT_TOKEN = "";
    QQ_HOST = "127.0.0.1";
    QQ_PORT = 3001;
    QQ_ACCESS_TOKEN = "";
    TELEGRAM_HOST = "api.telegram.org";
    TELEGRAM_PORT = 443;
    PROXY_HOST = "127.0.0.1";
    PROXY_PORT = 20122;
    PROXY_TYPE = "http";
    DATABASE_FILE = "bridge_bot.db";
    ENABLE_MINIAPP_PARSING = true;
    SHOW_RAW_JSON_ON_PARSE_FAIL = true;
    MAX_JSON_DISPLAY_LENGTH = 2000;
    ENABLE_RETRY_QUEUE = true;
    MESSAGE_RETRY_MAX_ATTEMPTS = 5;
    MEDIA_RETRY_MAX_ATTEMPTS = 3;
    MESSAGE_RETRY_BASE_INTERVAL_SEC = 2;
    MEDIA_RETRY_BASE_INTERVAL_SEC = 5;
    RETRY_QUEUE_CHECK_INTERVAL_SEC = 10;
    MAX_RETRY_INTERVAL_SEC = 300;
    BRIDGE_FILES_DIR = "/tmp/bridge_files";
    BRIDGE_FILES_CONTAINER_DIR = "/root/llonebot/bridge_files";
    GIF_MAX_FILE_SIZE = 0;
    GIF_MAX_DURATION = 5;
    GIF_MAX_FPS = 0;
    GIF_MAX_WIDTH = 0;
    GIF_MAX_COLORS = 256;
    IMAGE_URL_PROBE_MAX_ATTEMPTS = 3;
    IMAGE_URL_PROBE_BASE_DELAY_MS = 500;
    IMAGE_URL_PROBE_TIMEOUT_MS = 5000;
    IMAGE_PLACEHOLDER_URL =
        "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";
  }
}

} // namespace config

} // namespace bridge

#include "config.hpp"

namespace bridge {

std::unordered_map<std::string, GroupBridgeConfig> GROUP_MAP;

namespace config {

std::string TELEGRAM_BOT_TOKEN;
std::string QQ_HOST = "127.0.0.1";
uint16_t QQ_PORT = 3001;
std::string QQ_ACCESS_TOKEN;
std::string TELEGRAM_HOST = "api.telegram.org";
uint16_t TELEGRAM_PORT = 443;
std::string PROXY_HOST = "127.0.0.1";
uint16_t PROXY_PORT = 20122;
std::string PROXY_TYPE = "http";
std::string DATABASE_FILE = "bridge_actor.db";
bool ENABLE_MINIAPP_PARSING = true;
bool SHOW_RAW_JSON_ON_PARSE_FAIL = true;
int MAX_JSON_DISPLAY_LENGTH = 2000;
bool ENABLE_RETRY_QUEUE = true;
int MESSAGE_RETRY_MAX_ATTEMPTS = 5;
int MEDIA_RETRY_MAX_ATTEMPTS = 3;
int MESSAGE_RETRY_BASE_INTERVAL_SEC = 2;
int MEDIA_RETRY_BASE_INTERVAL_SEC = 5;
int RETRY_QUEUE_CHECK_INTERVAL_SEC = 10;
int MAX_RETRY_INTERVAL_SEC = 300;
std::string BRIDGE_FILES_DIR = "/tmp/bridge_files";
std::string BRIDGE_FILES_CONTAINER_DIR = "/root/llonebot/bridge_files";
size_t GIF_MAX_FILE_SIZE = 0;
int GIF_MAX_DURATION = 5;
int GIF_MAX_FPS = 0;
int GIF_MAX_WIDTH = 0;
int GIF_MAX_COLORS = 256;
int IMAGE_URL_PROBE_MAX_ATTEMPTS = 3;
int IMAGE_URL_PROBE_BASE_DELAY_MS = 500;
int IMAGE_URL_PROBE_TIMEOUT_MS = 5000;
std::string IMAGE_PLACEHOLDER_URL =
    "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";

} // namespace config

void apply_runtime_config(const BridgeRuntimeConfig &runtime_config) {
  GROUP_MAP = runtime_config.group_mappings;
  config::DATABASE_FILE = runtime_config.database_file;
  config::ENABLE_MINIAPP_PARSING = runtime_config.enable_miniapp_parsing;
  config::SHOW_RAW_JSON_ON_PARSE_FAIL =
      runtime_config.show_raw_json_on_parse_fail;
  config::MAX_JSON_DISPLAY_LENGTH = runtime_config.max_json_display_length;
  config::ENABLE_RETRY_QUEUE = runtime_config.enable_retry_queue;
  config::MESSAGE_RETRY_MAX_ATTEMPTS =
      runtime_config.message_retry_max_attempts;
  config::MEDIA_RETRY_MAX_ATTEMPTS = runtime_config.media_retry_max_attempts;
  config::MESSAGE_RETRY_BASE_INTERVAL_SEC =
      runtime_config.message_retry_base_interval_sec;
  config::MEDIA_RETRY_BASE_INTERVAL_SEC =
      runtime_config.media_retry_base_interval_sec;
  config::RETRY_QUEUE_CHECK_INTERVAL_SEC =
      runtime_config.retry_queue_check_interval_sec;
  config::MAX_RETRY_INTERVAL_SEC = runtime_config.max_retry_interval_sec;
  config::BRIDGE_FILES_DIR = runtime_config.bridge_files_dir;
  config::BRIDGE_FILES_CONTAINER_DIR =
      runtime_config.bridge_files_container_dir;
  config::GIF_MAX_FILE_SIZE = runtime_config.gif_max_file_size;
  config::GIF_MAX_DURATION = runtime_config.gif_max_duration;
  config::GIF_MAX_FPS = runtime_config.gif_max_fps;
  config::GIF_MAX_WIDTH = runtime_config.gif_max_width;
  config::GIF_MAX_COLORS = runtime_config.gif_max_colors;
  config::IMAGE_URL_PROBE_MAX_ATTEMPTS =
      runtime_config.image_url_probe_max_attempts;
  config::IMAGE_URL_PROBE_BASE_DELAY_MS =
      runtime_config.image_url_probe_base_delay_ms;
  config::IMAGE_URL_PROBE_TIMEOUT_MS =
      runtime_config.image_url_probe_timeout_ms;
  config::IMAGE_PLACEHOLDER_URL = runtime_config.image_placeholder_url;
}

} // namespace bridge

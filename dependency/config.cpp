#include "config.hpp"
#include "common/config_loader.hpp"
#include "common/logger.hpp"

namespace bridge {

// 全局群组映射配置
std::unordered_map<std::string, GroupBridgeConfig> GROUP_MAP;

void load_group_mappings() {
  try {
    GROUP_MAP.clear();

    // 获取配置
    auto config_section =
        obcx::common::ConfigLoader::instance().get_section("group_mappings");
    if (!config_section.has_value()) {
      PLUGIN_WARN("bridge", "No group_mappings section found in config");
      return;
    }
    const auto &config = config_section.value();

    // 加载群组到群组的映射
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
            PLUGIN_INFO("bridge", "Loaded group mapping: {} -> {}",
                        telegram_group_id, qq_group_id);
          }
        }
      }
    }

    // 加载Topic到群组的映射
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

            // 查找对应的topics -
            // 从全局topics数组中查找属于当前telegram_group_id的topics
            if (config.contains("topics")) {
              const auto &topics_array = config["topics"];

              if (topics_array.is_array()) {
                for (const auto &topic_item : *topics_array.as_array()) {
                  if (!topic_item.is_table())
                    continue;
                  const auto &topic_table = *topic_item.as_table();

                  // 检查这个topic是否属于当前的telegram_group_id
                  std::string topic_telegram_group_id =
                      topic_table["telegram_group_id"].value_or<std::string>(
                          "");

                  if (topic_telegram_group_id != telegram_group_id) {
                    continue; // 跳过不属于当前群的topic
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
                    PLUGIN_INFO("bridge", "Loaded topic mapping: {}:{} -> {}",
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
              PLUGIN_INFO("bridge",
                          "Loaded topic group mapping for TG {} with {} topics",
                          telegram_group_id, topics.size());
            }
          }
        }
      }
    }

    PLUGIN_INFO("bridge", "Group mappings loaded: {} total mappings",
                GROUP_MAP.size());
  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge", "Failed to load group mappings: {}", e.what());
  }
}

void initialize_config() {
  config::load_config();
  load_group_mappings();
}

namespace config {

// Bot tokens
std::string TELEGRAM_BOT_TOKEN;

// QQ服务器配置
std::string QQ_HOST;
uint16_t QQ_PORT;
std::string QQ_ACCESS_TOKEN;

// Telegram服务器配置
std::string TELEGRAM_HOST;
uint16_t TELEGRAM_PORT;

// 代理配置
std::string PROXY_HOST;
uint16_t PROXY_PORT;
std::string PROXY_TYPE;

// 数据库配置
std::string DATABASE_FILE;

// 超时配置
int REQUEST_TIMEOUT_MS;

// 小程序处理配置
bool ENABLE_MINIAPP_PARSING;
bool SHOW_RAW_JSON_ON_PARSE_FAIL;
int MAX_JSON_DISPLAY_LENGTH;

// 重试队列配置
bool ENABLE_RETRY_QUEUE;
int MESSAGE_RETRY_MAX_ATTEMPTS;
int MEDIA_RETRY_MAX_ATTEMPTS;
int MESSAGE_RETRY_BASE_INTERVAL_SEC;
int MEDIA_RETRY_BASE_INTERVAL_SEC;
int RETRY_QUEUE_CHECK_INTERVAL_SEC;
int MAX_RETRY_INTERVAL_SEC;

// 文件存储路径配置
std::string BRIDGE_FILES_DIR;
std::string BRIDGE_FILES_CONTAINER_DIR;

void load_config() {
  try {
    auto &loader = obcx::common::ConfigLoader::instance();
    PLUGIN_DEBUG("bridge", "Config file path: {}", loader.get_config_path());
    PLUGIN_DEBUG("bridge", "Config loaded: {}", loader.is_loaded());

    // 加载 Telegram Bot Token
    if (auto telegram_bot =
            loader.get_section("bots.telegram_bot.connection")) {
      TELEGRAM_BOT_TOKEN =
          telegram_bot->get("access_token")->value_or<std::string>("");
      TELEGRAM_HOST =
          telegram_bot->get("host")->value_or<std::string>("api.telegram.org");
      TELEGRAM_PORT = telegram_bot->get("port")->value_or<uint16_t>(443);

      // 代理配置（可选）
      PROXY_HOST =
          telegram_bot->get("proxy_host")->value_or<std::string>("127.0.0.1");
      PROXY_PORT = telegram_bot->get("proxy_port")->value_or<uint16_t>(20122);
      PROXY_TYPE =
          telegram_bot->get("proxy_type")->value_or<std::string>("http");
    }

    // 加载 QQ Bot 配置
    if (auto qq_bot = loader.get_section("bots.qq_bot.connection")) {
      QQ_HOST = qq_bot->get("host")->value_or<std::string>("127.0.0.1");
      QQ_PORT = qq_bot->get("port")->value_or<uint16_t>(3001);
      QQ_ACCESS_TOKEN = qq_bot->get("access_token")->value_or<std::string>("");
      REQUEST_TIMEOUT_MS = qq_bot->get("timeout")->value_or<int>(30000);
    }

    // 设置默认值
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

    // 从插件配置加载数据库配置
    // 尝试从 qq_to_tg 或 tg_to_qq 插件配置中读取
    bool config_loaded = false;

    // 先尝试从 qq_to_tg 读取
    // 注意：需要使用 at_path 访问嵌套配置
    if (auto qq_to_tg_config = loader.get_value<std::string>(
            "plugins.qq_to_tg.config.database_file")) {
      PLUGIN_DEBUG("bridge", "Found plugins.qq_to_tg.config section");
      DATABASE_FILE = *qq_to_tg_config;
    }
    if (auto retry = loader.get_value<bool>(
            "plugins.qq_to_tg.config.enable_retry_queue")) {
      ENABLE_RETRY_QUEUE = *retry;
    }

    // bridge_files_dir 是必需的配置
    if (auto dir = loader.get_value<std::string>(
            "plugins.qq_to_tg.config.bridge_files_dir")) {
      BRIDGE_FILES_DIR = *dir;
      config_loaded = true;
      PLUGIN_INFO("bridge", "Loaded bridge_files_dir from qq_to_tg: {}",
                  BRIDGE_FILES_DIR);
    } else {
      PLUGIN_DEBUG("bridge", "bridge_files_dir not found in qq_to_tg.config");
    }

    if (auto dir = loader.get_value<std::string>(
            "plugins.qq_to_tg.config.bridge_files_container_dir")) {
      BRIDGE_FILES_CONTAINER_DIR = *dir;
    }

    // 再尝试从 tg_to_qq 读取
    if (auto db = loader.get_value<std::string>(
            "plugins.tg_to_qq.config.database_file")) {
      if (DATABASE_FILE.empty()) {
        DATABASE_FILE = *db;
      }
    }
    if (auto retry = loader.get_value<bool>(
            "plugins.tg_to_qq.config.enable_retry_queue")) {
      ENABLE_RETRY_QUEUE = *retry;
    }

    // 如果 qq_to_tg 没有配置 bridge_files_dir，从 tg_to_qq 读取
    if (!config_loaded) {
      if (auto dir = loader.get_value<std::string>(
              "plugins.tg_to_qq.config.bridge_files_dir")) {
        BRIDGE_FILES_DIR = *dir;
        config_loaded = true;
        PLUGIN_INFO("bridge", "Loaded bridge_files_dir from tg_to_qq: {}",
                    BRIDGE_FILES_DIR);
      } else {
        PLUGIN_DEBUG("bridge", "bridge_files_dir not found in tg_to_qq.config");
      }

      if (auto dir = loader.get_value<std::string>(
              "plugins.tg_to_qq.config.bridge_files_container_dir")) {
        BRIDGE_FILES_CONTAINER_DIR = *dir;
      }
    }

    // 如果两个插件配置都没有提供 bridge_files_dir，报错
    if (!config_loaded) {
      throw std::runtime_error(
          "bridge_files_dir must be configured in either "
          "plugins.qq_to_tg.config or plugins.tg_to_qq.config");
    }

    PLUGIN_INFO("bridge", "Configuration loaded successfully");
    PLUGIN_INFO("bridge", "Telegram Bot Token: {}...",
                TELEGRAM_BOT_TOKEN.substr(0, 20));
    PLUGIN_INFO("bridge", "QQ Host: {}:{}", QQ_HOST, QQ_PORT);
    PLUGIN_INFO("bridge", "Database: {}", DATABASE_FILE);
    PLUGIN_INFO("bridge", "Bridge files dir (host): {}", BRIDGE_FILES_DIR);
    PLUGIN_INFO("bridge", "Bridge files dir (container): {}",
                BRIDGE_FILES_CONTAINER_DIR);

  } catch (const std::exception &e) {
    PLUGIN_ERROR("bridge", "Failed to load configuration: {}", e.what());
    // 设置默认值
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
    REQUEST_TIMEOUT_MS = 30000;
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
  }
}

} // namespace config

} // namespace bridge

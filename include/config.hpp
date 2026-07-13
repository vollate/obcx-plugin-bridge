#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

/**
 * @brief 桥接模式枚举
 */
enum class BridgeMode {
  GROUP_TO_GROUP, // 群组对群组模式（默认）
  TOPIC_TO_GROUP  // topic对群组模式
};

/**
 * @brief Topic桥接配置结构
 */
struct TopicBridgeConfig {
  int64_t telegram_topic_id; // Telegram topic ID
  std::string qq_group_id;   // 对应的QQ群ID
  bool show_qq_to_tg_sender; // QQ到TG显示发送者
  bool show_tg_to_qq_sender; // TG到QQ显示发送者
  bool enable_qq_to_tg;      // 启用QQ到TG转发
  bool enable_tg_to_qq;      // 启用TG到QQ转发

  TopicBridgeConfig(int64_t topic_id, const std::string &qq_id,
                    bool qq_to_tg = true, bool tg_to_qq = true,
                    bool enable_qq_tg = true, bool enable_tg_qq = true)
      : telegram_topic_id{topic_id}, qq_group_id{qq_id},
        show_qq_to_tg_sender{qq_to_tg}, show_tg_to_qq_sender{tg_to_qq},
        enable_qq_to_tg{enable_qq_tg}, enable_tg_to_qq{enable_tg_qq} {}
};

/**
 * @brief 群组桥接配置结构
 */
struct GroupBridgeConfig {
  std::string telegram_group_id; // Telegram群ID
  BridgeMode mode;               // 桥接模式

  // 群组模式配置
  std::string qq_group_id; // QQ群ID (群组模式)

  // Topic模式配置
  std::vector<TopicBridgeConfig> topics; // topic配置列表

  bool show_qq_to_tg_sender; // QQ到Telegram显示发送者
  bool show_tg_to_qq_sender; // Telegram到QQ显示发送者
  bool enable_qq_to_tg;      // 启用QQ到TG转发
  bool enable_tg_to_qq;      // 启用TG到QQ转发

  // 默认构造函数
  GroupBridgeConfig() = default;

  // 群组模式构造函数
  GroupBridgeConfig(const std::string &tg_id, const std::string &qq_id,
                    bool qq_to_tg = true, bool tg_to_qq = true,
                    bool enable_qq_tg = true, bool enable_tg_qq = true)
      : telegram_group_id{tg_id}, mode{BridgeMode::GROUP_TO_GROUP},
        qq_group_id{qq_id}, show_qq_to_tg_sender{qq_to_tg},
        show_tg_to_qq_sender{tg_to_qq}, enable_qq_to_tg{enable_qq_tg},
        enable_tg_to_qq{enable_tg_qq} {}

  // Topic模式构造函数
  GroupBridgeConfig(const std::string &tg_id,
                    const std::vector<TopicBridgeConfig> &topic_configs,
                    bool qq_to_tg = true, bool tg_to_qq = true,
                    bool enable_qq_tg = true, bool enable_tg_qq = true)
      : telegram_group_id{tg_id}, mode{BridgeMode::TOPIC_TO_GROUP},
        topics{topic_configs}, show_qq_to_tg_sender{qq_to_tg},
        show_tg_to_qq_sender{tg_to_qq}, enable_qq_to_tg{enable_qq_tg},
        enable_tg_to_qq{enable_tg_qq} {}
};

struct BridgeRuntimeConfig {
  std::unordered_map<std::string, GroupBridgeConfig> group_mappings;
  std::string database_file = "bridge_actor.db";
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
  std::string bridge_files_dir = "/tmp/bridge_files";
  std::string bridge_files_container_dir = "/root/llonebot/bridge_files";
  size_t gif_max_file_size = 0;
  int gif_max_duration = 5;
  int gif_max_fps = 0;
  int gif_max_width = 0;
  int gif_max_colors = 256;
  int image_url_probe_max_attempts = 3;
  int image_url_probe_base_delay_ms = 500;
  int image_url_probe_timeout_ms = 5000;
  std::string image_placeholder_url =
      "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";
};

void apply_runtime_config(const BridgeRuntimeConfig &runtime_config);

// Actor-owned group bridge mappings.
extern std::unordered_map<std::string, GroupBridgeConfig> GROUP_MAP;

// 辅助函数
namespace {
/**
 * @brief 根据Telegram群ID和Topic ID查找对应的QQ群ID
 */
inline std::string get_qq_group_id_for_topic(const std::string &tg_group_id,
                                             int64_t topic_id) {
  auto it = GROUP_MAP.find(tg_group_id);
  if (it == GROUP_MAP.end())
    return "";

  const auto &config = it->second;
  if (config.mode == BridgeMode::GROUP_TO_GROUP) {
    return config.qq_group_id;
  } else {
    // Topic模式：查找对应的topic配置
    for (const auto &topic_config : config.topics) {
      if (topic_config.telegram_topic_id == topic_id) {
        return topic_config.qq_group_id;
      }
    }
  }
  return "";
}

/**
 * @brief 根据QQ群ID查找对应的Telegram群ID和Topic ID
 */
inline std::pair<std::string, int64_t> get_tg_group_and_topic_id(
    const std::string &qq_group_id) {
  for (const auto &[tg_id, config] : GROUP_MAP) {
    if (config.mode == BridgeMode::GROUP_TO_GROUP) {
      if (config.qq_group_id == qq_group_id) {
        return {tg_id, -1}; // -1表示不是topic模式
      }
    } else {
      // Topic模式：查找对应的topic配置
      for (const auto &topic_config : config.topics) {
        if (topic_config.qq_group_id == qq_group_id) {
          return {tg_id, topic_config.telegram_topic_id};
        }
      }
    }
  }
  return {"", -1};
}

/**
 * @brief 获取群组桥接配置
 */
inline const GroupBridgeConfig *get_bridge_config(
    const std::string &tg_group_id) {
  auto it = GROUP_MAP.find(tg_group_id);
  return (it != GROUP_MAP.end()) ? &it->second : nullptr;
}

/**
 * @brief 根据Telegram群ID和Topic ID获取Topic配置
 */
const TopicBridgeConfig *get_topic_config(const std::string &tg_group_id,
                                          int64_t topic_id) {
  auto it = GROUP_MAP.find(tg_group_id);
  if (it == GROUP_MAP.end() || it->second.mode != BridgeMode::TOPIC_TO_GROUP) {
    return nullptr;
  }

  for (const auto &topic_config : it->second.topics) {
    if (topic_config.telegram_topic_id == topic_id) {
      return &topic_config;
    }
  }
  return nullptr;
}

} // namespace

// 动态配置变量
namespace config {
// Bot tokens
extern std::string TELEGRAM_BOT_TOKEN;

// QQ服务器配置
extern std::string QQ_HOST;
extern uint16_t QQ_PORT;
extern std::string QQ_ACCESS_TOKEN;

// Telegram服务器配置
extern std::string TELEGRAM_HOST;
extern uint16_t TELEGRAM_PORT;

// 代理配置
extern std::string PROXY_HOST;
extern uint16_t PROXY_PORT;
extern std::string PROXY_TYPE;

// 数据库配置
extern std::string DATABASE_FILE;

// 小程序处理配置
extern bool ENABLE_MINIAPP_PARSING;
extern bool SHOW_RAW_JSON_ON_PARSE_FAIL;
extern int MAX_JSON_DISPLAY_LENGTH;

// 重试队列配置
extern bool ENABLE_RETRY_QUEUE;
extern int MESSAGE_RETRY_MAX_ATTEMPTS;
extern int MEDIA_RETRY_MAX_ATTEMPTS;
extern int MESSAGE_RETRY_BASE_INTERVAL_SEC;
extern int MEDIA_RETRY_BASE_INTERVAL_SEC;
extern int RETRY_QUEUE_CHECK_INTERVAL_SEC;
extern int MAX_RETRY_INTERVAL_SEC;

// 文件存储路径配置
extern std::string BRIDGE_FILES_DIR;           // 主机端路径
extern std::string BRIDGE_FILES_CONTAINER_DIR; // 容器端路径

// GIF转换配置
extern size_t GIF_MAX_FILE_SIZE; // 最大GIF文件大小（字节），默认0=不限
extern int GIF_MAX_DURATION;     // 最大转换时长（秒），默认5
extern int GIF_MAX_FPS;          // 最大帧率（0=不限），默认0
extern int GIF_MAX_WIDTH;        // 最大宽度（0=不限），默认0
extern int GIF_MAX_COLORS;       // 最大调色板颜色数，默认256

// QQ→TG 图片URL预校验配置
// 在调用 sendMediaGroup 之前，先用 HTTP 探测 QQ 图片URL是否可达，
// 单张失败时按指数退避重试若干次；如果仍失败则替换为占位图，
// 这样不会因为某一张图阻塞整批 MediaGroup 的发送。
extern int IMAGE_URL_PROBE_MAX_ATTEMPTS;  // 单张URL最多探测次数，默认3
extern int IMAGE_URL_PROBE_BASE_DELAY_MS; // 退避基准延迟（毫秒），默认500
extern int IMAGE_URL_PROBE_TIMEOUT_MS;    // 单次探测超时（毫秒），默认5000
extern std::string IMAGE_PLACEHOLDER_URL; // 失败兜底占位图URL（空=丢弃）

} // namespace config

} // namespace bridge

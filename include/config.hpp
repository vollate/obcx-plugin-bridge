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

// 动态加载的群组桥接映射配置
extern std::unordered_map<std::string, GroupBridgeConfig> GROUP_MAP;

/**
 * @brief 从配置文件加载群组映射
 */
void load_group_mappings();

/**
 * @brief 初始化配置系统
 */
void initialize_config();

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

// 向后兼容的简单函数
inline std::string get_qq_group_id(const std::string &tg_group_id) {
  return get_qq_group_id_for_topic(tg_group_id, -1);
}

inline std::string get_tg_group_id(const std::string &qq_group_id) {
  return get_tg_group_and_topic_id(qq_group_id).first;
}
} // namespace

// 兼容性别名：保持原有的简单映射接口（仅适用于群组模式）
inline const std::unordered_map<std::string, std::string>
get_legacy_group_map() {
  static std::unordered_map<std::string, std::string> legacy_map;
  if (legacy_map.empty()) {
    for (const auto &[tg_id, config] : GROUP_MAP) {
      if (config.mode == BridgeMode::GROUP_TO_GROUP) {
        legacy_map[tg_id] = config.qq_group_id;
      }
    }
  }
  return legacy_map;
}

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

// 超时配置
extern int REQUEST_TIMEOUT_MS;

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

/**
 * @brief 从配置文件加载配置
 */
void load_config();
} // namespace config

} // namespace bridge

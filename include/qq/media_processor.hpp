#pragma once

#include "config.hpp"
#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <interfaces/bot.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bridge::qq {

/**
 * @brief 小程序解析结果结构
 */
struct MiniAppParseResult {
  bool success = false;
  std::string title;
  std::string description;
  std::vector<std::string> urls;
  std::string app_name;
  std::string raw_json;
};

/**
 * @brief QQ媒体文件处理器
 *
 * 处理QQ到Telegram的媒体文件转换和处理
 */
class QQMediaProcessor {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit QQMediaProcessor(
      std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 处理QQ消息段并转换为Telegram格式
   * @param qq_bot QQ机器人实例
   * @param telegram_bot Telegram机器人实例
   * @param segment QQ消息段
   * @param event 原始消息事件
   * @param telegram_group_id Telegram群ID
   * @param topic_id Topic ID
   * @param sender_display_name 发送者显示名称
   * @param bridge_config 桥接配置
   * @param temp_files_to_cleanup 临时文件清理列表
   * @return 转换后的Telegram消息段（可能为空表示已直接发送）
   */
  auto process_qq_media_segment(obcx::core::IBot &qq_bot,
                                obcx::core::IBot &telegram_bot,
                                const obcx::common::MessageSegment &segment,
                                const obcx::common::MessageEvent &event,
                                const std::string &telegram_group_id,
                                int64_t topic_id,
                                const std::string &sender_display_name,
                                const GroupBridgeConfig *bridge_config,
                                std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>>;

  /**
   * @brief 处理图片段（包含表情包检测和缓存）
   */
  auto process_image_segment(obcx::core::IBot &qq_bot,
                             obcx::core::IBot &telegram_bot,
                             const obcx::common::MessageSegment &segment,
                             const obcx::common::MessageEvent &event,
                             const std::string &telegram_group_id,
                             int64_t topic_id,
                             const std::string &sender_display_name,
                             const GroupBridgeConfig *bridge_config,
                             std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::optional<obcx::common::MessageSegment>>;

  /**
   * @brief 处理文件段（包含URL获取）
   */
  auto process_file_segment(obcx::core::IBot &qq_bot,
                            const obcx::common::MessageSegment &segment,
                            const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理@消息段（包含用户信息同步）
   */
  auto process_at_segment(obcx::core::IBot &qq_bot,
                          const obcx::common::MessageSegment &segment,
                          const obcx::common::MessageEvent &event,
                          const std::string &telegram_group_id,
                          int64_t topic_id,
                          const GroupBridgeConfig *bridge_config)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理语音段
   */
  static auto process_record_segment(
      const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理视频段
   */
  static auto process_video_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理QQ表情段
   */
  static auto process_face_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理QQ超级表情/表情包段 (mface)
   */
  static auto process_mface_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理戳一戳段
   */
  static auto process_shake_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理音乐分享段
   */
  static auto process_music_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理链接分享段
   */
  static auto process_share_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理JSON小程序段
   */
  static auto process_json_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理应用分享段
   */
  static auto process_app_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理ARK卡片段
   */
  static auto process_ark_segment(const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理小程序段
   */
  static auto process_miniapp_segment(
      const obcx::common::MessageSegment &segment)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 检测图片是否为GIF格式
   */
  auto detect_gif_format(const std::string &url)
      -> boost::asio::awaitable<bool>;

  /**
   * @brief 检测是否为表情包
   */
  static auto is_sticker(const obcx::common::MessageSegment &segment) -> bool;

  /**
   * @brief 处理表情包缓存
   */
  auto handle_sticker_cache(obcx::core::IBot &telegram_bot,
                            const obcx::common::MessageSegment &segment,
                            const std::string &telegram_group_id,
                            int64_t topic_id,
                            const std::string &sender_display_name,
                            const GroupBridgeConfig *bridge_config)
      -> boost::asio::awaitable<bool>;

  /**
   * @brief 解析小程序JSON数据
   */
  static auto parse_miniapp_json(const std::string &json_data)
      -> MiniAppParseResult;

  /**
   * @brief 格式化小程序消息段
   */
  static auto format_miniapp_message(const MiniAppParseResult &parse_result)
      -> obcx::common::MessageSegment;

  /**
   * @brief 从JSON字符串中提取URLs
   */
  static auto extract_urls_from_json(const std::string &json_str)
      -> std::vector<std::string>;

  /**
   * @brief 将二进制数据转换为16进制字符串用于调试
   */
  static auto to_hex_string(const std::string &data, size_t max_bytes = 16)
      -> std::string;
};

} // namespace bridge::qq

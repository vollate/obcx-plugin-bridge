#pragma once

#include "database/manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/tg_bot.hpp>
#include <interfaces/bot.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

namespace bridge::telegram {

/**
 * @brief Telegram媒体文件处理器
 *
 * 处理各种类型的Telegram媒体文件下载、缓存和转换
 */
class TelegramMediaProcessor {
public:
  /**
   * @brief 构造函数
   * @param db_manager 数据库管理器
   */
  explicit TelegramMediaProcessor(
      std::shared_ptr<storage::DatabaseManager> db_manager);

  /**
   * @brief 处理Telegram媒体文件并转换为QQ消息段
   * @param telegram_bot Telegram机器人实例
   * @param file_type 文件类型
   * @param file_id 文件ID
   * @param media_data 媒体文件JSON数据
   * @param temp_files_to_cleanup 临时文件清理列表
   * @return 处理后的消息段列表
   */
  auto process_media_file(obcx::core::IBot &telegram_bot,
                          const std::string &file_type,
                          const std::string &file_id,
                          const nlohmann::json &media_data,
                          std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::vector<obcx::common::MessageSegment>>;

  /**
   * @brief 下载并缓存Telegram贴纸到本地
   * @param telegram_bot Telegram机器人实例
   * @param media_info 媒体文件信息
   * @param bridge_files_dir 文件存储目录
   * @return 本地文件路径的awaitable，失败时返回nullopt
   */
  auto download_sticker_with_cache(
      obcx::core::IBot &telegram_bot,
      const obcx::core::MediaFileInfo &media_info,
      const std::string &bridge_files_dir = "/tmp/bridge_files")
      -> boost::asio::awaitable<std::optional<std::string>>;

  /**
   * @brief 下载并缓存Telegram动画到本地，自动进行webm到gif的转换
   * @param telegram_bot Telegram机器人实例
   * @param media_info 媒体文件信息
   * @param bridge_files_dir 文件存储目录
   * @return 本地文件路径的awaitable，失败时返回nullopt
   */
  auto download_animation_with_cache(
      obcx::core::IBot &telegram_bot,
      const obcx::core::MediaFileInfo &media_info,
      const std::string &bridge_files_dir = "/tmp/bridge_files")
      -> boost::asio::awaitable<std::optional<std::string>>;

private:
  std::shared_ptr<storage::DatabaseManager> db_manager_;

  /**
   * @brief 处理图片文件
   */
  auto process_photo(obcx::core::IBot &telegram_bot,
                     const std::string &file_url, const std::string &filename,
                     std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理视频文件
   */
  auto process_video(obcx::core::IBot &telegram_bot,
                     const std::string &file_url, const std::string &filename,
                     std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理音频文件
   */
  auto process_audio(obcx::core::IBot &telegram_bot,
                     const std::string &file_url, const std::string &filename,
                     std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理文档文件
   */
  auto process_document(obcx::core::IBot &telegram_bot,
                        const std::string &file_url,
                        const std::string &filename,
                        std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理贴纸文件
   */
  auto process_sticker(obcx::core::IBot &telegram_bot,
                       const obcx::core::MediaFileInfo &media_info,
                       const nlohmann::json &media_data)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理动画文件
   */
  auto process_animation(obcx::core::IBot &telegram_bot,
                         const obcx::core::MediaFileInfo &media_info,
                         const nlohmann::json &media_data,
                         const std::string &filename)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理视频消息
   */
  auto process_video_note(obcx::core::IBot &telegram_bot,
                          const std::string &file_url,
                          const std::string &filename,
                          std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  /**
   * @brief 处理其他类型文件
   */
  auto process_other_file(obcx::core::IBot &telegram_bot,
                          const std::string &file_url,
                          const std::string &filename,
                          std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
};

} // namespace bridge::telegram

// Telegram媒体处理器：普通文件下载与QQ消息段转换。
//
// 包括 photo、video、audio/voice、document、video_note 和兜底文件处理。

#include "telegram/media_processor.hpp"

#include "media_processor.hpp"

#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <telegram/network/connection_manager.hpp>

namespace bridge::telegram {

auto TelegramMediaProcessor::process_photo(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "photo", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "image";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["proxy"] = 1;
      PLUGIN_INFO("tg_to_qq", "成功下载图片到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载图片失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载图片失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "photo");
    file_segment.type = "image";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_video(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "video", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载视频到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载视频失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载视频失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "video");
    file_segment.type = "video";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_audio(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "audio", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "record";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载音频到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载音频失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载音频失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "audio");
    file_segment.type = "record";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_document(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "document", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      PLUGIN_INFO("tg_to_qq", "成功下载文档到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载文档失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载文档失败，回退到URL方式: {}", e.what());
    auto [final_url, _] =
        MediaProcessor::get_qq_file_info(file_url, "document");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_video_note(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "video_note", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      PLUGIN_INFO("tg_to_qq", "成功下载视频消息到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载视频消息失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载视频消息失败，回退到URL方式: {}", e.what());
    auto [final_url, _] =
        MediaProcessor::get_qq_file_info(file_url, "video_note");
    file_segment.type = "video";
    file_segment.data["file"] = final_url;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

auto TelegramMediaProcessor::process_other_file(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto *tg_bot = &dynamic_cast<obcx::core::TGBot &>(telegram_bot);
    auto *conn_manager =
        dynamic_cast<obcx::network::TelegramConnectionManager *>(
            tg_bot->get_connection_manager());
    if (!conn_manager) {
      throw std::runtime_error("无法获取TelegramConnectionManager实例");
    }

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        conn_manager, file_url, "file", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      const auto &path_manager = MediaProcessor::get_path_manager();
      std::string container_path =
          path_manager.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      PLUGIN_INFO("tg_to_qq", "成功下载其他类型文件到本地: {} -> 容器路径: {}",
                  local_file_path, container_path);
    } else {
      throw std::runtime_error("下载其他类型文件失败");
    }
  } catch (const std::exception &e) {
    PLUGIN_WARN("tg_to_qq", "下载其他类型文件失败，回退到URL方式: {}",
                e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "file");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

} // namespace bridge::telegram

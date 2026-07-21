#include "telegram/media_processor.hpp"

#include "media_processor.hpp"

#include <common/logger.hpp>
#include <interfaces/telegram_bot.hpp>

namespace bridge::telegram {

auto TelegramMediaProcessor::process_photo(
    obcx::core::IBot &telegram_bot, const std::string &file_url,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment file_segment;

  try {
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "photo", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "image";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["proxy"] = 1;
      OBCX_INFO("成功下载图片到本地: {} -> 容器路径: {}", local_file_path,
                container_path);
    } else {
      throw std::runtime_error("下载图片失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载图片失败，回退到URL方式: {}", e.what());
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
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "video", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      OBCX_INFO("成功下载视频到本地: {} -> 容器路径: {}", local_file_path,
                container_path);
    } else {
      throw std::runtime_error("下载视频失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载视频失败，回退到URL方式: {}", e.what());
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
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "audio", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "record";
      file_segment.data["file"] = "file:///" + container_path;
      OBCX_INFO("成功下载音频到本地: {} -> 容器路径: {}", local_file_path,
                container_path);
    } else {
      throw std::runtime_error("下载音频失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载音频失败，回退到URL方式: {}", e.what());
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
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "document", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      OBCX_INFO("成功下载文档到本地: {} -> 容器路径: {}", local_file_path,
                container_path);
    } else {
      throw std::runtime_error("下载文档失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载文档失败，回退到URL方式: {}", e.what());
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
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "video_note", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "video";
      file_segment.data["file"] = "file:///" + container_path;
      OBCX_INFO("成功下载视频消息到本地: {} -> 容器路径: {}", local_file_path,
                container_path);
    } else {
      throw std::runtime_error("下载视频消息失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载视频消息失败，回退到URL方式: {}", e.what());
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
    auto &telegram_capability =
        dynamic_cast<obcx::core::ITelegramBot &>(telegram_bot);

    auto local_path_opt = co_await MediaProcessor::download_media_file(
        &telegram_capability, path_manager_, file_url, "file", filename);

    if (local_path_opt.has_value()) {
      std::string local_file_path = local_path_opt.value();
      temp_files_to_cleanup.push_back(local_file_path);

      std::string container_path =
          path_manager_.host_to_container_absolute(local_file_path);

      file_segment.type = "file";
      file_segment.data["file"] = "file:///" + container_path;
      file_segment.data["name"] = filename;
      OBCX_INFO("成功下载其他类型文件到本地: {} -> 容器路径: {}",
                local_file_path, container_path);
    } else {
      throw std::runtime_error("下载其他类型文件失败");
    }
  } catch (const std::exception &e) {
    OBCX_WARN("下载其他类型文件失败，回退到URL方式: {}", e.what());
    auto [final_url, _] = MediaProcessor::get_qq_file_info(file_url, "file");
    file_segment.type = "file";
    file_segment.data["file"] = final_url;
    file_segment.data["name"] = filename;
    file_segment.data["proxy"] = 1;
  }

  co_return file_segment;
}

} // namespace bridge::telegram

#include "telegram/media_processor.hpp"

#include "bridge_state_repository.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>
#include <utility>

namespace bridge::telegram {
namespace {

auto display_name(const std::string_view type) -> std::string_view {
  if (type == "photo" || type == "image") {
    return "图片";
  }
  if (type == "video") {
    return "视频";
  }
  if (type == "audio") {
    return "音频";
  }
  if (type == "voice") {
    return "语音";
  }
  if (type == "sticker") {
    return "贴纸";
  }
  if (type == "animation") {
    return "GIF动画";
  }
  if (type == "video_note") {
    return "视频消息";
  }
  if (type == "document") {
    return "文档";
  }
  return "文件";
}

} // namespace

TelegramMediaProcessor::TelegramMediaProcessor(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor)
    : operations_(std::move(operations)), config_(std::move(config)),
      path_manager_(config_->bridge_files_dir,
                    config_->bridge_files_container_dir),
      state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)) {
  if (!operations_) {
    throw std::invalid_argument(
        "TelegramMediaProcessor requires bot operations");
  }
}

auto TelegramMediaProcessor::process_media_file(
    const std::string &file_type, const std::string &file_id,
    const nlohmann::json &media_data,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::vector<obcx::common::MessageSegment>> {
  std::vector<obcx::common::MessageSegment> result;
  try {
    if (file_id.empty()) {
      result.push_back(
          {.type = "text",
           .data = {{"text", fmt::format("[{}]", display_name(file_type))}}});
      co_return result;
    }

    obcx::bot::TelegramFileRef file{.file_id = file_id, .file_type = file_type};
    const nlohmann::json *metadata = nullptr;
    if (media_data.contains(file_type) && media_data[file_type].is_object()) {
      metadata = &media_data[file_type];
    }
    if (metadata != nullptr) {
      if (metadata->contains("file_size") &&
          (*metadata)["file_size"].is_number_integer()) {
        file.file_size = (*metadata)["file_size"].get<std::int64_t>();
      }
      if (metadata->contains("file_unique_id") &&
          (*metadata)["file_unique_id"].is_string()) {
        file.file_unique_id = (*metadata)["file_unique_id"].get<std::string>();
      }
      if (metadata->contains("mime_type") &&
          (*metadata)["mime_type"].is_string()) {
        file.mime_type = (*metadata)["mime_type"].get<std::string>();
      }
      if (metadata->contains("file_name") &&
          (*metadata)["file_name"].is_string()) {
        file.file_name = (*metadata)["file_name"].get<std::string>();
      }
    }

    obcx::common::MessageSegment segment;
    if (file_type == "sticker") {
      if (media_data.contains("sticker")) {
        const auto &sticker = media_data["sticker"];
        if (sticker.value("is_animated", false)) {
          file.mime_type = "application/tgs";
        } else if (sticker.value("is_video", false)) {
          file.mime_type = "video/webm";
        } else {
          file.mime_type = "image/webp";
        }
      }
      segment = co_await process_sticker(file, media_data);
    } else if (file_type == "animation") {
      segment = co_await process_animation(
          file, media_data, file.file_name.value_or("animation"));
    } else {
      auto fetched = co_await operations_->fetch_telegram_file(
          file, config_->qq_media_download_max_bytes);
      std::string output_type = "file";
      if (file_type == "photo" || file_type == "image") {
        output_type = "image";
      } else if (file_type == "video" || file_type == "video_note") {
        output_type = "video";
      } else if (file_type == "audio" || file_type == "voice") {
        output_type = "record";
      }
      segment = co_await process_downloaded_file(
          fetched, output_type,
          file.file_name.value_or(file_type + "-" + file_id),
          temp_files_to_cleanup);
    }
    result.push_back(std::move(segment));

    if (media_data.contains("caption") && media_data["caption"].is_string() &&
        !media_data["caption"].get<std::string>().empty()) {
      result.push_back(
          {.type = "text",
           .data = {{"text", media_data["caption"].get<std::string>()}}});
    }
  } catch (const std::exception &error) {
    OBCX_ERROR("处理媒体文件失败: {}", error.what());
  }
  co_return result;
}

} // namespace bridge::telegram

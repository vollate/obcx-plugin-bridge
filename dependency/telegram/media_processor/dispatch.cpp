#include "telegram/media_processor.hpp"

#include "media_processor.hpp"

#include <common/logger.hpp>
#include <core/tg_bot.hpp>
#include <fmt/format.h>
#include <utility>

namespace bridge::telegram {

TelegramMediaProcessor::TelegramMediaProcessor(
    std::shared_ptr<storage::DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

auto TelegramMediaProcessor::process_media_file(
    obcx::core::IBot &telegram_bot, const std::string &file_type,
    const std::string &file_id, const nlohmann::json &media_data,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<std::vector<obcx::common::MessageSegment>> {

  std::vector<obcx::common::MessageSegment> result;

  try {
    if (file_id.empty()) {
      obcx::common::MessageSegment text_segment;
      text_segment.type = "text";
      std::string type_name = file_type == "photo" || file_type == "image"
                                  ? "图片"
                              : file_type == "video"      ? "视频"
                              : file_type == "audio"      ? "音频"
                              : file_type == "voice"      ? "语音"
                              : file_type == "sticker"    ? "贴纸"
                              : file_type == "animation"  ? "GIF动画"
                              : file_type == "video_note" ? "视频消息"
                              : file_type == "document"   ? "文档"
                                                          : "文件";
      text_segment.data["text"] = fmt::format("[{}]", type_name);
      result.push_back(text_segment);
      co_return result;
    }

    obcx::core::MediaFileInfo media_info;
    media_info.file_id = file_id;
    media_info.file_type = file_type;

    auto download_url_opt =
        co_await dynamic_cast<obcx::core::TGBot &>(telegram_bot)
            .get_media_download_url(media_info);
    if (!download_url_opt.has_value()) {
      throw std::runtime_error("无法获取文件下载链接");
    }

    const std::string &file_url = download_url_opt.value();
    auto [final_url, filename] =
        MediaProcessor::get_qq_file_info(file_url, file_type);

    obcx::common::MessageSegment file_segment;

    if (file_type == "photo" || file_type == "image") {
      file_segment = co_await process_photo(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "video") {
      file_segment = co_await process_video(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "audio" || file_type == "voice") {
      file_segment = co_await process_audio(telegram_bot, file_url, filename,
                                            temp_files_to_cleanup);
    } else if (file_type == "document") {
      file_segment = co_await process_document(telegram_bot, file_url, filename,
                                               temp_files_to_cleanup);
    } else if (file_type == "sticker") {
      // 从 media_data 补齐 MediaFileInfo 中下载链接接口未提供的字段
      if (media_data.contains("sticker")) {
        auto sticker = media_data["sticker"];
        if (sticker.contains("file_size")) {
          media_info.file_size = sticker["file_size"].get<int64_t>();
        }
        if (sticker.contains("file_unique_id")) {
          media_info.file_unique_id =
              sticker["file_unique_id"].get<std::string>();
        }
        if (sticker.contains("is_animated") &&
            sticker["is_animated"].get<bool>()) {
          media_info.mime_type = "application/tgs";
        } else if (sticker.contains("is_video") &&
                   sticker["is_video"].get<bool>()) {
          media_info.mime_type = "video/webm";
        } else {
          media_info.mime_type = "image/webp";
        }
      }
      file_segment =
          co_await process_sticker(telegram_bot, media_info, media_data);
    } else if (file_type == "animation") {
      if (media_data.contains("animation")) {
        auto animation = media_data["animation"];
        if (animation.contains("file_unique_id")) {
          media_info.file_unique_id =
              animation["file_unique_id"].get<std::string>();
        }
      }
      file_segment = co_await process_animation(telegram_bot, media_info,
                                                media_data, filename);
    } else if (file_type == "video_note") {
      file_segment = co_await process_video_note(
          telegram_bot, file_url, filename, temp_files_to_cleanup);
    } else {
      file_segment = co_await process_other_file(
          telegram_bot, file_url, filename, temp_files_to_cleanup);
    }

    result.push_back(file_segment);
    if (media_data.contains("caption") &&
        !media_data["caption"].get<std::string>().empty()) {
      nlohmann::json caption_text;
      caption_text["type"] = "text";
      caption_text["text"] = media_data["caption"].get<std::string>();
      result.push_back(obcx::common::MessageSegment{
          .type = "text", .data = std::move(caption_text)});
    }
    PLUGIN_INFO("tg_to_qq", "成功处理Telegram {}文件: {}", file_type, filename);

  } catch (const std::exception &e) {
    PLUGIN_ERROR("tg_to_qq", "处理媒体文件失败: {}", e.what());
  }

  co_return result;
}

} // namespace bridge::telegram

#include "qq/media_processor.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::qq {

auto QQMediaProcessor::process_record_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted = segment;
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  OBCX_DEBUG("转发QQ语音文件: file={}, url={}", file_name, url);

  if (!url.empty()) {
    converted.data["file"] = url;
  }

  co_return converted;
}

auto QQMediaProcessor::process_video_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted = segment;
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  OBCX_DEBUG("转发QQ视频文件: file={}, url={}", file_name, url);

  if (!url.empty()) {
    converted.data["file"] = url;
  }

  co_return converted;
}

auto QQMediaProcessor::process_face_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string face_id = segment.data.value("id", "0");
  converted.data.clear();
  converted.data["text"] = fmt::format("[QQ表情:{}]", face_id);
  OBCX_DEBUG("转换QQ表情为文本提示: face_id={}", face_id);
  co_return converted;
}

auto QQMediaProcessor::process_mface_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;

  // mface 是 QQ 超级表情，绝大多数实际是 GIF——直接按 animation 走能保留动画。
  std::string url = segment.data.value("url", "");
  std::string summary = segment.data.value("summary", "");
  std::string emoji_id = segment.data.value("emoji_id", "");

  if (!url.empty()) {
    converted.type = "animation";
    converted.data["file"] = url;

    if (!summary.empty()) {
      converted.data["caption"] = summary;
    }

    OBCX_DEBUG("转换QQ超级表情为动画: url={}, summary={}, emoji_id={}", url,
               summary, emoji_id);
  } else {
    converted.type = "text";
    converted.data.clear();
    if (!summary.empty()) {
      converted.data["text"] = fmt::format("[表情包:{}]", summary);
    } else {
      converted.data["text"] = "[QQ超级表情]";
    }
    OBCX_WARN("QQ超级表情缺少URL，转换为文本: summary={}", summary);
  }

  co_return converted;
}

auto QQMediaProcessor::process_shake_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  converted.data.clear();
  converted.data["text"] = "[戳一戳]";
  OBCX_DEBUG("转换QQ戳一戳为文本提示");
  co_return converted;
}

auto QQMediaProcessor::process_music_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string title = segment.data.value("title", "未知音乐");
  converted.data.clear();
  converted.data["text"] = fmt::format("[音乐分享: {}]", title);
  OBCX_DEBUG("转换QQ音乐分享为文本: title={}", title);
  co_return converted;
}

auto QQMediaProcessor::process_share_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;
  converted.type = "text";
  std::string url = segment.data.value("url", "");
  std::string title = segment.data.value("title", "链接分享");
  converted.data.clear();
  converted.data["text"] = fmt::format("[{}]\t{}", title, url);
  OBCX_DEBUG("转换QQ链接分享为文本: title={}, url={}", title, url);
  co_return converted;
}

} // namespace bridge::qq

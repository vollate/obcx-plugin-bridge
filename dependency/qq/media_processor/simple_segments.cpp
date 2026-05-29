// QQ媒体处理器：简单的消息段转换。
//
// 这里集中处理那些转换逻辑非常简单的消息段：
//   - record  语音
//   - video   视频
//   - face    QQ 自带表情（id 提示）
//   - mface   超级表情/表情包（带URL，转 animation）
//   - shake   戳一戳
//   - music   音乐分享
//   - share   链接分享

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

  PLUGIN_DEBUG("qq_to_tg", "转发QQ语音文件: file={}, url={}", file_name, url);

  // 优先使用URL进行远程下载
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

  PLUGIN_DEBUG("qq_to_tg", "转发QQ视频文件: file={}, url={}", file_name, url);

  // 优先使用URL进行远程下载
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
  PLUGIN_DEBUG("qq_to_tg", "转换QQ表情为文本提示: face_id={}", face_id);
  co_return converted;
}

auto QQMediaProcessor::process_mface_segment(
    const obcx::common::MessageSegment &segment)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted;

  // mface通常包含GIF表情包，提取URL和summary
  std::string url = segment.data.value("url", "");
  std::string summary = segment.data.value("summary", "");
  std::string emoji_id = segment.data.value("emoji_id", "");

  if (!url.empty()) {
    // 大部分QQ超级表情都是GIF格式，转换为Telegram的animation类型
    converted.type = "animation";
    converted.data["file"] = url;

    // 如果有summary，作为caption
    if (!summary.empty()) {
      converted.data["caption"] = summary;
    }

    PLUGIN_DEBUG("qq_to_tg",
                 "转换QQ超级表情为动画: url={}, summary={}, emoji_id={}", url,
                 summary, emoji_id);
  } else {
    // 如果没有URL，降级为文本提示
    converted.type = "text";
    converted.data.clear();
    if (!summary.empty()) {
      converted.data["text"] = fmt::format("[表情包:{}]", summary);
    } else {
      converted.data["text"] = "[QQ超级表情]";
    }
    PLUGIN_WARN("qq_to_tg", "QQ超级表情缺少URL，转换为文本: summary={}",
                summary);
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
  PLUGIN_DEBUG("qq_to_tg", "转换QQ戳一戳为文本提示");
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
  PLUGIN_DEBUG("qq_to_tg", "转换QQ音乐分享为文本: title={}", title);
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
  PLUGIN_DEBUG("qq_to_tg", "转换QQ链接分享为文本: title={}, url={}", title,
               url);
  co_return converted;
}

} // namespace bridge::qq

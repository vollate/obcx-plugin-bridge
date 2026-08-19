// QQ file 段三档优先级：
//   1) 上报里直接带 url -> 直接转发
//   2) 仅有 file_id -> 调 get_group_file_url / get_private_file_url 拿下载链接
//   3) 都没有 -> 降级为文本提示

#include "qq/media_processor.hpp"

#include <common/logger.hpp>
#include <fmt/format.h>

namespace bridge::qq {

auto QQMediaProcessor::process_file_segment(
    const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment = segment;
  converted_segment.type = "document";

  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  OBCX_DEBUG("转发QQ文件: file={}, url={}", file_name, url);

  std::string file_id = segment.data.value("file_id", "");
  std::string file_size = segment.data.value("file_size", "");

  if (!url.empty()) {
    converted_segment.data["file"] = url;
    OBCX_DEBUG("使用QQ文件URL进行转发: {}", url);
  } else if (!file_id.empty()) {
    OBCX_DEBUG("URL为空，尝试通过file_id获取文件: {}", file_id);
    try {
      std::string download_url;
      if (event.group_id.has_value()) {
        download_url = co_await operations_->resolve_onebot11_group_file(
            *event.group_id, file_id);
      } else {
        download_url = co_await operations_->resolve_onebot11_private_file(
            event.user_id, file_id);
      }
      converted_segment.data.erase("file_id");
      converted_segment.data["url"] = download_url;
    } catch (const std::exception &e) {
      OBCX_WARN("通过API获取文件URL失败: {}", e.what());
      converted_segment.type = "text";
      converted_segment.data.clear();
      converted_segment.data["text"] = fmt::format(
          "[文件: {} ({} bytes)]\n❌ 无法获取下载链接", file_name, file_size);
    }
  } else {
    converted_segment.type = "text";
    converted_segment.data.clear();
    converted_segment.data["text"] = fmt::format(
        "[文件: {} ({} bytes)]\n❌ 缺少文件信息", file_name, file_size);
    OBCX_WARN("QQ文件缺少URL和file_id信息: {}", file_name);
  }

  co_return converted_segment;
}

} // namespace bridge::qq

// QQ媒体处理器：文件段。
//
// QQ消息中的 file 段需要特殊处理：
//   - 优先使用上报的 url 字段直接转发
//   - 若没有 url 但有 file_id，则按群聊/私聊分别调用
//     get_group_file_url / get_private_file_url 获取下载链接
//   - 都获取不到时降级为文本提示

#include "qq/media_processor.hpp"

#include <common/logger.hpp>
#include <core/qq_bot.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace bridge::qq {

auto QQMediaProcessor::process_file_segment(
    obcx::core::IBot &qq_bot, const obcx::common::MessageSegment &segment,
    const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {

  obcx::common::MessageSegment converted_segment = segment;
  converted_segment.type = "document";

  // 提取QQ文件的file和url信息
  std::string file_name = segment.data.value("file", "");
  std::string url = segment.data.value("url", "");

  PLUGIN_DEBUG("qq_to_tg", "转发QQ文件: file={}, url={}", file_name, url);

  // 提取更多信息用于诊断
  std::string file_id = segment.data.value("file_id", "");
  std::string file_size = segment.data.value("file_size", "");

  if (!url.empty()) {
    // 有URL时使用远程下载
    converted_segment.data["file"] = url;
    PLUGIN_DEBUG("qq_to_tg", "使用QQ文件URL进行转发: {}", url);
  } else if (!file_id.empty()) {
    // URL为空但有file_id时，使用LLOneBot的文件URL获取API
    PLUGIN_DEBUG("qq_to_tg", "URL为空，尝试通过file_id获取文件: {}", file_id);
    try {
      std::string response;
      // 根据消息来源选择API：群聊使用get_group_file_url，私聊使用get_private_file_url
      auto *qq_bot_ptr = dynamic_cast<obcx::core::QQBot *>(&qq_bot);
      if (event.group_id.has_value()) {
        // 群聊文件
        std::string group_id = event.group_id.value();
        response = co_await qq_bot_ptr->get_group_file_url(group_id, file_id);
        PLUGIN_DEBUG("qq_to_tg", "get_group_file_url API响应: {}", response);
      } else {
        // 私聊文件
        std::string user_id = event.user_id;
        response = co_await qq_bot_ptr->get_private_file_url(user_id, file_id);
        PLUGIN_DEBUG("qq_to_tg", "get_private_file_url API响应: {}", response);
      }

      nlohmann::json response_json = nlohmann::json::parse(response);

      if (response_json.contains("status") && response_json["status"] == "ok" &&
          response_json.contains("data") &&
          response_json["data"].contains("url")) {
        std::string download_url = response_json["data"]["url"];
        converted_segment.data.erase("file_id");
        converted_segment.data["url"] = download_url;
        PLUGIN_DEBUG("qq_to_tg", "成功通过API获取文件下载URL: {}",
                     download_url);
      } else {
        throw std::runtime_error("API响应中没有找到下载URL");
      }
    } catch (const std::exception &e) {
      PLUGIN_WARN("qq_to_tg", "通过API获取文件URL失败: {}", e.what());
      // 转换为错误提示
      converted_segment.type = "text";
      converted_segment.data.clear();
      converted_segment.data["text"] = fmt::format(
          "[文件: {} ({} bytes)]\n❌ 无法获取下载链接", file_name, file_size);
    }
  } else {
    // 既没有URL也没有file_id
    converted_segment.type = "text";
    converted_segment.data.clear();
    converted_segment.data["text"] = fmt::format(
        "[文件: {} ({} bytes)]\n❌ 缺少文件信息", file_name, file_size);
    PLUGIN_WARN("qq_to_tg", "QQ文件缺少URL和file_id信息: {}", file_name);
  }

  co_return converted_segment;
}

} // namespace bridge::qq

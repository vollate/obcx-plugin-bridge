#include "telegram/media_processor.hpp"

#include <common/logger.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace bridge::telegram {

auto TelegramMediaProcessor::process_downloaded_file(
    const obcx::bot::FetchedTelegramFile &file, std::string output_type,
    const std::string &filename,
    std::vector<std::string> &temp_files_to_cleanup)
    -> boost::asio::awaitable<obcx::common::MessageSegment> {
  const auto safe_name = std::filesystem::path{filename}.filename().string();
  if (safe_name.empty()) {
    throw std::runtime_error("Telegram media filename is empty");
  }
  const auto relative =
      "temp/" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "_" + safe_name;
  const auto local_path = co_await blocking_executor_->run(
      [path_manager = path_manager_, relative, bytes = file.bytes] {
        if (!path_manager.ensure_directory("temp")) {
          throw std::runtime_error("cannot create Telegram media directory");
        }
        const auto path = path_manager.to_host_path(relative);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
          throw std::runtime_error("cannot write Telegram media file");
        }
        return path;
      });
  temp_files_to_cleanup.push_back(local_path);
  const auto container_path = co_await blocking_executor_->run(
      [path_manager = path_manager_, local_path] {
        return path_manager.host_to_container_absolute(local_path);
      });

  obcx::common::MessageSegment segment;
  segment.type = std::move(output_type);
  segment.data["file"] = "file:///" + container_path;
  if (segment.type == "image") {
    segment.data["proxy"] = 1;
  }
  if (segment.type == "file") {
    segment.data["name"] = safe_name;
  }
  co_return segment;
}

} // namespace bridge::telegram

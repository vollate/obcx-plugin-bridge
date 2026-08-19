#pragma once

#include "bridge_bot_operations.hpp"
#include "config.hpp"
#include "path_manager.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace bridge {
class BridgeStateRepository;
}

namespace bridge::telegram {

class TelegramMediaProcessor {
public:
  explicit TelegramMediaProcessor(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const bridge::BridgeConfig> config,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor);

  auto process_media_file(const std::string &file_type,
                          const std::string &file_id,
                          const nlohmann::json &media_data,
                          std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<std::vector<obcx::common::MessageSegment>>;
  auto download_sticker_with_cache(
      const obcx::bot::TelegramFileRef &media_info,
      const std::string &bridge_files_dir = "/tmp/bridge_files")
      -> boost::asio::awaitable<std::optional<std::string>>;
  auto download_animation_with_cache(
      const obcx::bot::TelegramFileRef &media_info,
      const std::string &bridge_files_dir = "/tmp/bridge_files")
      -> boost::asio::awaitable<std::optional<std::string>>;

private:
  auto process_downloaded_file(const obcx::bot::FetchedTelegramFile &file,
                               std::string output_type,
                               const std::string &filename,
                               std::vector<std::string> &temp_files_to_cleanup)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_sticker(const obcx::bot::TelegramFileRef &media_info,
                       const nlohmann::json &media_data)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;
  auto process_animation(const obcx::bot::TelegramFileRef &media_info,
                         const nlohmann::json &media_data,
                         const std::string &filename)
      -> boost::asio::awaitable<obcx::common::MessageSegment>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const bridge::BridgeConfig> config_;
  PathManager path_manager_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
};

} // namespace bridge::telegram

#pragma once

#include "bridge_bot_operations.hpp"
#include "config.hpp"
#include "qq/image_url_validator.hpp"
#include "qq/photo_normalizer.hpp"

#include <boost/asio.hpp>
#include <common/message_type.hpp>
#include <core/blocking_executor.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bridge {
class BridgeStateRepository;
class ReceivedMessageRepository;
struct BridgeConfig;
} // namespace bridge

namespace bridge::qq {

struct MediaGroupSendResult {
  bool sent = false;
  std::optional<std::string> primary_target_message_id;
};

struct PreparedMedia {
  std::string type;
  std::string url;
  std::size_t original_index{0};
  bool replaced{false};
};

struct MediaGroupFallbackResult {
  std::optional<obcx::bot::SendMessageResult> send_result;
  bool used_multipart{false};
  std::size_t replaced_count{0};
  std::size_t normalized_count{0};
};

class QQMessageFormatter {
public:
  using ImageDownloader =
      std::function<boost::asio::awaitable<std::vector<MediaDownloadResult>>(
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &)>;
  using ImageSanitizer = std::function<
      boost::asio::awaitable<std::vector<std::pair<std::string, std::string>>>(
          const bridge::BridgeConfig &,
          const std::vector<std::pair<std::string, std::string>> &,
          std::vector<std::string> &)>;
  using ImageNormalizer = std::function<std::vector<PhotoNormalizationResult>(
      const bridge::BridgeConfig &, std::vector<DownloadedImage>)>;

  explicit QQMessageFormatter(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<const bridge::BridgeConfig> config,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository = nullptr,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor = nullptr,
      ImageDownloader image_downloader = {},
      ImageSanitizer image_sanitizer = {},
      ImageNormalizer image_normalizer = {});

  auto format_sender_info(const obcx::common::MessageEvent &event,
                          const GroupBridgeConfig *bridge_config,
                          const std::string &qq_group_id,
                          const std::string &telegram_group_id,
                          std::int64_t topic_id,
                          obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<std::string>;
  auto format_reply_message(const obcx::common::MessageEvent &event,
                            obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<bool>;
  auto process_forward_message(const obcx::common::MessageSegment &segment,
                               const std::string &telegram_group_id,
                               std::int64_t topic_id,
                               obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<void>;
  auto process_node_message(const obcx::common::MessageSegment &segment,
                            obcx::common::Message &message_to_send)
      -> boost::asio::awaitable<void>;
  void set_received_message_repository(
      std::shared_ptr<bridge::ReceivedMessageRepository> repository) {
    received_message_repository_ = std::move(repository);
  }

  auto send_media_group(
      const std::vector<obcx::common::MessageSegment> &image_segments,
      const std::vector<obcx::common::MessageSegment> &other_segments,
      const std::string &telegram_group_id, std::int64_t topic_id,
      const std::string &sender_display_name,
      const GroupBridgeConfig *bridge_config,
      const obcx::common::Message &message_to_send,
      const obcx::common::MessageEvent &event)
      -> boost::asio::awaitable<MediaGroupSendResult>;

  static auto fetch_and_save_user_info(
      std::shared_ptr<BridgeBotOperations> operations,
      std::shared_ptr<bridge::BridgeStateRepository> state_repository,
      std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
      const std::string &user_id, const std::string &group_id)
      -> boost::asio::awaitable<void>;

private:
  auto send_media_group_with_fallback(
      std::string_view telegram_group_id,
      const std::vector<PreparedMedia> &media, std::string_view caption,
      std::optional<std::int64_t> topic_id,
      std::optional<std::string> reply_to_message_id,
      const std::vector<obcx::bot::TelegramTextEntity> &caption_entities = {})
      -> boost::asio::awaitable<MediaGroupFallbackResult>;
  auto get_user_display_name(const std::string &user_id,
                             const std::string &group_id)
      -> boost::asio::awaitable<std::string>;
  auto fetch_user_info(const std::string &user_id, const std::string &group_id)
      -> boost::asio::awaitable<void>;

  std::shared_ptr<BridgeBotOperations> operations_;
  std::shared_ptr<const bridge::BridgeConfig> config_;
  std::shared_ptr<bridge::BridgeStateRepository> state_repository_;
  std::shared_ptr<bridge::ReceivedMessageRepository>
      received_message_repository_;
  std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor_;
  ImageDownloader image_downloader_;
  ImageSanitizer image_sanitizer_;
  ImageNormalizer image_normalizer_;
};

} // namespace bridge::qq

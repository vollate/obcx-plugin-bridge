#pragma once

#include <boost/asio/awaitable.hpp>
#include <common/message_type.hpp>
#include <core/bot_operation_client.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bridge {

class BridgeBotOperationFailure final : public std::runtime_error {
public:
  explicit BridgeBotOperationFailure(obcx::bot::BotOperationError error)
      : std::runtime_error(error.message), error_(std::move(error)) {}

  [[nodiscard]] auto error() const noexcept
      -> const obcx::bot::BotOperationError & {
    return error_;
  }

private:
  obcx::bot::BotOperationError error_;
};

struct BridgeOperationFailureDisposition {
  bool retryable = false;
  bool outcome_unknown = false;
  std::string diagnostic;
};

[[nodiscard]] auto classify_bridge_operation_failure(
    const std::exception &error) -> BridgeOperationFailureDisposition;

class BridgeBotOperations final {
public:
  BridgeBotOperations(std::shared_ptr<obcx::bot::BotOperationClient> client,
                      std::string telegram_installation,
                      std::string onebot11_installation);

  [[nodiscard]] auto telegram_installation() const noexcept
      -> const obcx::bot::BotInstallationRef &;
  [[nodiscard]] auto onebot11_installation() const noexcept
      -> const obcx::bot::BotInstallationRef &;

  [[nodiscard]] auto telegram_group(std::string group_id) const
      -> obcx::bot::GroupTarget;
  [[nodiscard]] auto onebot11_group(std::string group_id) const
      -> obcx::bot::GroupTarget;

  auto send_telegram_group(std::string_view group_id,
                           const obcx::common::Message &message)
      -> boost::asio::awaitable<std::string>;
  auto send_telegram_topic(std::string_view group_id, std::int64_t topic_id,
                           const obcx::common::Message &message)
      -> boost::asio::awaitable<std::string>;
  auto send_onebot11_group(std::string_view group_id,
                           const obcx::common::Message &message)
      -> boost::asio::awaitable<std::string>;

  auto delete_telegram_message(std::string_view group_id,
                               std::string_view message_id)
      -> boost::asio::awaitable<void>;
  auto delete_onebot11_message(std::string_view group_id,
                               std::string_view message_id)
      -> boost::asio::awaitable<void>;
  auto edit_telegram_message(std::string_view group_id,
                             std::string_view message_id, std::string_view text,
                             std::string_view parse_mode = {})
      -> boost::asio::awaitable<void>;

  auto send_telegram_photo(
      std::string_view group_id, std::string photo, std::string caption,
      std::vector<obcx::bot::TelegramTextEntity> entities = {})
      -> boost::asio::awaitable<std::string>;
  auto send_telegram_media_urls(
      std::string_view group_id,
      std::vector<obcx::bot::TelegramMediaSource> media, std::string caption,
      std::optional<std::int64_t> topic_id = std::nullopt,
      std::optional<std::string> reply_to_message_id = std::nullopt,
      std::vector<obcx::bot::TelegramTextEntity> entities = {})
      -> boost::asio::awaitable<obcx::bot::SendMessageResult>;
  auto send_telegram_media_uploads(
      std::string_view group_id,
      std::vector<obcx::bot::TelegramMediaUpload> media, std::string caption,
      std::size_t maximum_bytes,
      std::optional<std::int64_t> topic_id = std::nullopt,
      std::optional<std::string> reply_to_message_id = std::nullopt,
      std::vector<obcx::bot::TelegramTextEntity> entities = {})
      -> boost::asio::awaitable<obcx::bot::SendMessageResult>;
  auto fetch_telegram_file(obcx::bot::TelegramFileRef file,
                           std::size_t maximum_bytes)
      -> boost::asio::awaitable<obcx::bot::FetchedTelegramFile>;

  auto get_onebot11_group_member(std::string_view group_id,
                                 std::string_view user_id,
                                 bool no_cache = false)
      -> boost::asio::awaitable<obcx::bot::OneBotGroupMember>;
  auto get_onebot11_forward_messages(std::string_view forward_id)
      -> boost::asio::awaitable<obcx::bot::Json>;
  auto resolve_onebot11_group_file(std::string_view group_id,
                                   std::string_view file_id)
      -> boost::asio::awaitable<std::string>;
  auto resolve_onebot11_private_file(std::string_view user_id,
                                     std::string_view file_id)
      -> boost::asio::awaitable<std::string>;
  auto poke_onebot11_group(std::string_view group_id, std::string_view user_id)
      -> boost::asio::awaitable<void>;

private:
  template <typename T>
  static auto require(obcx::bot::BotOperationResult<T> result) -> T {
    result.validate();
    if (!result.ok()) {
      throw BridgeBotOperationFailure(std::move(*result.error));
    }
    return std::move(*result.value);
  }

  std::shared_ptr<obcx::bot::BotOperationClient> client_;
  obcx::bot::BotInstallationRef telegram_;
  obcx::bot::BotInstallationRef onebot11_;
};

} // namespace bridge

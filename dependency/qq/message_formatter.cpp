#include "qq/message_formatter.hpp"

#include "bridge_state_repository.hpp"
#include "qq/image_url_validator.hpp"
#include "received_message_repository.hpp"

#include <common/json_utils.hpp>
#include <common/logger.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>

namespace bridge::qq {

namespace {

namespace fs = std::filesystem;

class TemporaryMediaUploads {
public:
  TemporaryMediaUploads() = default;
  TemporaryMediaUploads(const TemporaryMediaUploads &) = delete;
  auto operator=(const TemporaryMediaUploads &)
      -> TemporaryMediaUploads & = delete;

  TemporaryMediaUploads(TemporaryMediaUploads &&other) noexcept
      : root_(std::move(other.root_)), uploads_(std::move(other.uploads_)) {
    other.root_.clear();
  }

  auto operator=(TemporaryMediaUploads &&other) noexcept
      -> TemporaryMediaUploads & {
    if (this != &other) {
      cleanup();
      root_ = std::move(other.root_);
      uploads_ = std::move(other.uploads_);
      other.root_.clear();
    }
    return *this;
  }

  ~TemporaryMediaUploads() { cleanup(); }

  static auto materialize(std::vector<DownloadedImage> downloaded)
      -> TemporaryMediaUploads {
    TemporaryMediaUploads result;
    result.root_ = fs::temp_directory_path() /
                   ("obcx-qq-media-" + boost::uuids::to_string(
                                           boost::uuids::random_generator{}()));
    std::error_code error;
    const bool created = fs::create_directory(result.root_, error);
    if (error || !created) {
      throw std::runtime_error("cannot create QQ media temporary directory");
    }

    result.uploads_.reserve(downloaded.size());
    for (auto &image : downloaded) {
      auto filename = fs::path{image.filename}.filename().string();
      if (filename.empty()) {
        throw std::runtime_error("QQ media temporary filename is empty");
      }
      const auto path = result.root_ / filename;
      {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(image.data.data(),
                     static_cast<std::streamsize>(image.data.size()));
        if (!output) {
          throw std::runtime_error("cannot write QQ media temporary file");
        }
      }

      std::ifstream input(path, std::ios::binary);
      std::string file_data{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
      if (input.bad() || file_data.size() != image.data.size()) {
        throw std::runtime_error("cannot read QQ media temporary file");
      }
      result.uploads_.push_back(obcx::bot::TelegramMediaUpload{
          .type = std::move(image.type),
          .filename = std::move(filename),
          .mime_type = std::move(image.mime_type),
          .bytes =
              std::vector<std::uint8_t>(file_data.begin(), file_data.end()),
      });
    }
    return result;
  }

  [[nodiscard]] auto uploads() const
      -> const std::vector<obcx::bot::TelegramMediaUpload> & {
    return uploads_;
  }

  [[nodiscard]] auto root() const -> const fs::path & { return root_; }

  void mark_cleaned() noexcept { root_.clear(); }

private:
  void cleanup() noexcept {
    if (root_.empty()) {
      return;
    }
    std::error_code ignored;
    fs::remove_all(root_, ignored);
    root_.clear();
  }

  fs::path root_;
  std::vector<obcx::bot::TelegramMediaUpload> uploads_;
};

auto is_telegram_bad_request(const std::exception &error) -> bool {
  if (const auto *typed =
          dynamic_cast<const BridgeBotOperationFailure *>(&error)) {
    return typed->error().code ==
               obcx::bot::BotOperationErrorCode::ProviderRejected &&
           typed->error().provider_code == "400";
  }
  return std::string_view{error.what()}.contains("HTTP request failed: 400");
}

auto is_operation_aborted(const std::exception &error) -> bool {
  const auto *system_error =
      dynamic_cast<const boost::system::system_error *>(&error);
  return system_error != nullptr &&
         system_error->code() == boost::asio::error::operation_aborted;
}

auto telegram_failure_category(std::string_view error) -> std::string_view {
  if (error.find("IMAGE_PROCESS_FAILED") != std::string_view::npos) {
    return "image_process_failed";
  }
  if (error.find("PHOTO_INVALID_DIMENSIONS") != std::string_view::npos) {
    return "invalid_dimensions";
  }
  if (error.find("failed to get HTTP URL content") != std::string_view::npos) {
    return "remote_url_unavailable";
  }
  if (error.find("body limit exceeded") != std::string_view::npos) {
    return "response_limit";
  }
  if (error.find("HTTP request failed: 400") != std::string_view::npos) {
    return "telegram_bad_request";
  }
  if (error.find("HTTP request failed: 401") != std::string_view::npos ||
      error.find("HTTP request failed: 403") != std::string_view::npos) {
    return "telegram_authorization";
  }
  if (error.find("HTTP request failed: 429") != std::string_view::npos) {
    return "telegram_rate_limited";
  }
  if (error.find("HTTP request failed: 5") != std::string_view::npos) {
    return "telegram_server_error";
  }
  if (error.find("timed out") != std::string_view::npos ||
      error.find("timeout") != std::string_view::npos) {
    return "timeout";
  }
  return "transport_or_runtime";
}

class MediaFallbackError final : public std::runtime_error {
public:
  MediaFallbackError(std::string stage, std::string category,
                     std::size_t item_count, std::size_t replaced_count,
                     std::size_t normalized_count = 0)
      : std::runtime_error(fmt::format(
            "stage={}, category={}, normalized={}, replaced={}/{}", stage,
            category, normalized_count, replaced_count, item_count)),
        stage_(std::move(stage)), category_(std::move(category)),
        item_count_(item_count), replaced_count_(replaced_count),
        normalized_count_(normalized_count) {}

  [[nodiscard]] auto stage() const -> std::string_view { return stage_; }
  [[nodiscard]] auto category() const -> std::string_view { return category_; }
  [[nodiscard]] auto item_count() const -> std::size_t { return item_count_; }
  [[nodiscard]] auto replaced_count() const -> std::size_t {
    return replaced_count_;
  }
  [[nodiscard]] auto normalized_count() const -> std::size_t {
    return normalized_count_;
  }

private:
  std::string stage_;
  std::string category_;
  std::size_t item_count_;
  std::size_t replaced_count_;
  std::size_t normalized_count_;
};

auto failure_name(MediaDownloadFailure failure) -> std::string_view {
  switch (failure) {
  case MediaDownloadFailure::InvalidUrl:
    return "invalid_url";
  case MediaDownloadFailure::Transport:
    return "transport";
  case MediaDownloadFailure::HttpStatus:
    return "http_status";
  case MediaDownloadFailure::OverLimit:
    return "over_limit";
  case MediaDownloadFailure::EmptyBody:
    return "empty_body";
  case MediaDownloadFailure::InvalidImage:
    return "invalid_image";
  case MediaDownloadFailure::InvalidDimensions:
    return "invalid_dimensions";
  case MediaDownloadFailure::UnsafeDimensions:
    return "unsafe_dimensions";
  case MediaDownloadFailure::NormalizationFailed:
    return "normalization_failed";
  case MediaDownloadFailure::None:
    break;
  }
  return "unknown";
}

auto media_failure(PhotoNormalizationFailure failure) -> MediaDownloadFailure {
  switch (failure) {
  case PhotoNormalizationFailure::InvalidDimensions:
    return MediaDownloadFailure::InvalidDimensions;
  case PhotoNormalizationFailure::UnsafeDimensions:
    return MediaDownloadFailure::UnsafeDimensions;
  case PhotoNormalizationFailure::NormalizationFailed:
    return MediaDownloadFailure::NormalizationFailed;
  case PhotoNormalizationFailure::None:
    break;
  }
  return MediaDownloadFailure::NormalizationFailed;
}

auto embedded_placeholder() -> DownloadedImage {
  // Final offline fallback for when the configured NOT FOUND image cannot be
  // downloaded. Keep this useful rather than falling back to a black pixel.
  static const std::string kPng = [] {
    static constexpr std::string_view kBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAQAAAABgCAYAAADy8ayIAAAByklEQVR42u3cMQ7CMAxA"
        "0V6ImWswMXFWjoSE1B1mWBhwXCd+w5ugAUXmD1Hpdj+dX0BPm00AAQAEAGgdgMdzBxYl"
        "ACAAAgACIAAgAAIAAiAAIAA2CQQAEABAAAABAAQAEABAAAABAAQAEABAAAABAAQAEABAA"
        "AABAAQAEABAAAABAAQAEABAAIDqAbhcbx9GX//9/l+i1/v386Jlf//R60XPT/b6VeZFA"
        "ARAAARAAARAAARAAARAAAQgIwDRA5AdmOjrswNQbf3qP9DZ51MABEAABEAABEAABEAABE"
        "AABKDSIeDo1wVAAI6cPwEQAAEQAAEQAAEQAAEQAAEQAAGIOzQUgLE3AnUPwOzzKQACIA"
        "ACIAACIAACIAACIAACsNKfgQTAIWDn9QVAAARAAARAAARAAARAAARAAARAAARAAARAAA"
        "RAAARAAARAAARAAARAADwUNHZDuwcge/9nv5HJU4EFQAAEQAAEQAAEQAAEQAAEYOUAAP"
        "UIAAiAAIAACAAIgACAAAgACIBNAgEABAAQAEAAAAEABAAQAEAAAAEABAAQAEAAAAEABAA"
        "QAEAAAAEABAAQAEAAAAEABAAQAGBUAIA+BAAEABAAoJU3yWqRE/JFpjwAAAAASUVORK5C"
        "YII=";
    const auto value = [](unsigned char byte) -> unsigned int {
      if (byte >= 'A' && byte <= 'Z') {
        return byte - 'A';
      }
      if (byte >= 'a' && byte <= 'z') {
        return byte - 'a' + 26U;
      }
      if (byte >= '0' && byte <= '9') {
        return byte - '0' + 52U;
      }
      return byte == '+' ? 62U : 63U;
    };

    std::string decoded;
    decoded.reserve(kBase64.size() * 3U / 4U);
    std::uint32_t buffer = 0;
    unsigned int bits = 0;
    for (const unsigned char byte : kBase64) {
      if (byte == '=') {
        break;
      }
      buffer = (buffer << 6U) | value(byte);
      bits += 6U;
      if (bits < 8U) {
        continue;
      }
      bits -= 8U;
      decoded.push_back(static_cast<char>((buffer >> bits) & 0xffU));
      buffer &= bits == 0U ? 0U : (1U << bits) - 1U;
    }
    return decoded;
  }();
  return DownloadedImage{
      .type = "photo",
      .filename = "qq-media-placeholder.png",
      .mime_type = "image/png",
      .data = kPng,
  };
}

void append_italic_telegram_sender(obcx::common::Message &message,
                                   std::string_view sender) {
  message.push_back(obcx::common::MessageSegment{
      .type = "text",
      .data = {{"text", fmt::format("[{}]", sender)},
               {"telegram_style", "italic"}}});
  message.push_back(
      obcx::common::MessageSegment{.type = "text", .data = {{"text", "\t"}}});
}

auto telegram_utf16_units(const std::string_view text) -> std::size_t {
  std::size_t units = 0;
  for (std::size_t index = 0; index < text.size();) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t width = 1;
    if ((lead & 0xE0U) == 0xC0U) {
      width = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      width = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      width = 4;
    }
    if (index + width > text.size()) {
      width = 1;
    }
    units += width == 4 ? 2U : 1U;
    index += width;
  }
  return units;
}

auto italic_entity(std::string_view text)
    -> std::vector<obcx::bot::TelegramTextEntity> {
  return {
      {.type = "italic", .offset = 0, .length = telegram_utf16_units(text)}};
}

auto caption_with_media_recovery(std::string_view caption,
                                 std::size_t replaced_count,
                                 std::size_t normalized_count) -> std::string {
  std::string result{caption};
  const auto append_line = [&result](std::string line) {
    if (!result.empty()) {
      result += "\n";
    }
    result += std::move(line);
  };
  if (normalized_count != 0) {
    append_line(fmt::format("ℹ️ {} 张图片尺寸已自动调整", normalized_count));
  }
  if (replaced_count != 0) {
    append_line(
        fmt::format("⚠️ {} 张图片暂时无法获取，已用占位图替换", replaced_count));
  }
  return result;
}

} // namespace

QQMessageFormatter::QQMessageFormatter(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<const bridge::BridgeConfig> config,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
    ImageDownloader image_downloader, ImageSanitizer image_sanitizer,
    ImageNormalizer image_normalizer)
    : operations_(std::move(operations)), config_(std::move(config)),
      state_repository_(std::move(state_repository)),
      blocking_executor_(std::move(blocking_executor)),
      image_downloader_(std::move(image_downloader)),
      image_sanitizer_(std::move(image_sanitizer)),
      image_normalizer_(std::move(image_normalizer)) {
  if (!operations_) {
    throw std::invalid_argument("QQMessageFormatter requires bot operations");
  }
  if (!image_downloader_) {
    image_downloader_ =
        [](const bridge::BridgeConfig &config,
           const std::vector<std::pair<std::string, std::string>> &media) {
          return ImageUrlValidator::download(config, media);
        };
  }
  if (!image_sanitizer_) {
    image_sanitizer_ =
        [](const bridge::BridgeConfig &config,
           const std::vector<std::pair<std::string, std::string>> &media,
           std::vector<std::string> &replaced) {
          return ImageUrlValidator::sanitize(config, media, replaced);
        };
  }
  if (!image_normalizer_) {
    image_normalizer_ = [](const bridge::BridgeConfig &config,
                           std::vector<DownloadedImage> images) {
      return PhotoNormalizer(config.ffmpeg_path,
                             config.qq_media_download_max_bytes)
          .normalize_batch(std::move(images));
    };
  }
}

auto QQMessageFormatter::send_media_group_with_fallback(
    std::string_view telegram_group_id, const std::vector<PreparedMedia> &media,
    std::string_view caption, std::optional<int64_t> topic_id,
    std::optional<std::string> reply_to_message_id,
    const std::vector<obcx::bot::TelegramTextEntity> &caption_entities)
    -> boost::asio::awaitable<MediaGroupFallbackResult> {
  std::vector<std::pair<std::string, std::string>> remote_media;
  remote_media.reserve(media.size());
  std::set<std::size_t> replaced_indices;
  for (const auto &item : media) {
    remote_media.emplace_back(item.type, item.url);
    if (item.replaced) {
      replaced_indices.insert(item.original_index);
    }
  }

  try {
    std::vector<obcx::bot::TelegramMediaSource> sources;
    sources.reserve(remote_media.size());
    for (const auto &[type, source] : remote_media) {
      sources.push_back({.type = type, .source = source});
    }
    auto response = co_await operations_->send_telegram_media_urls(
        telegram_group_id, std::move(sources),
        caption_with_media_recovery(caption, replaced_indices.size(), 0),
        topic_id, reply_to_message_id, caption_entities);
    co_return MediaGroupFallbackResult{
        .send_result = std::move(response),
        .replaced_count = replaced_indices.size(),
    };
  } catch (const std::exception &error) {
    if (is_operation_aborted(error)) {
      throw;
    }
    const auto category = telegram_failure_category(error.what());
    if (!is_telegram_bad_request(error)) {
      throw MediaFallbackError("direct_url_send", std::string{category},
                               media.size(), replaced_indices.size());
    }
    OBCX_WARN("MediaGroup URL 发送被 Telegram 拒绝，改用 multipart 上传: "
              "category={}, items={}, pre_replaced={}",
              category, media.size(), replaced_indices.size());
  }

  if (!blocking_executor_) {
    throw MediaFallbackError("fallback_setup", "blocking_executor_unavailable",
                             media.size(), replaced_indices.size());
  }
  std::vector<MediaDownloadResult> outcomes(media.size());
  std::vector<std::pair<std::string, std::string>> source_downloads;
  std::vector<std::size_t> source_indices;
  for (std::size_t index = 0; index < media.size(); ++index) {
    if (media[index].replaced) {
      outcomes[index].failure = MediaDownloadFailure::InvalidUrl;
      outcomes[index].diagnostic = "media URL was replaced during validation";
      continue;
    }
    source_downloads.push_back(remote_media[index]);
    source_indices.push_back(index);
  }

  if (!source_downloads.empty()) {
    std::vector<MediaDownloadResult> source_outcomes;
    try {
      source_outcomes = co_await image_downloader_(*config_, source_downloads);
    } catch (const std::exception &error) {
      if (is_operation_aborted(error)) {
        throw;
      }
      throw MediaFallbackError(
          "media_download",
          std::string{telegram_failure_category(error.what())}, media.size(),
          replaced_indices.size());
    }
    if (source_outcomes.size() != source_downloads.size()) {
      throw MediaFallbackError("media_download", "incomplete_outcomes",
                               media.size(), replaced_indices.size());
    }
    for (std::size_t index = 0; index < source_outcomes.size(); ++index) {
      outcomes[source_indices[index]] = std::move(source_outcomes[index]);
    }
  }

  std::size_t normalized_count = 0;
  std::vector<DownloadedImage> normalization_candidates;
  std::vector<std::size_t> normalization_indices;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    if (!outcomes[index].succeeded()) {
      continue;
    }
    normalization_candidates.push_back(std::move(*outcomes[index].image));
    outcomes[index].image.reset();
    normalization_indices.push_back(index);
  }
  if (!normalization_candidates.empty()) {
    std::vector<PhotoNormalizationResult> normalized;
    try {
      normalized = co_await blocking_executor_->run(
          [normalizer = image_normalizer_, config = config_,
           candidates = std::move(normalization_candidates)]() mutable {
            return normalizer(*config, std::move(candidates));
          });
    } catch (const std::exception &error) {
      if (is_operation_aborted(error)) {
        throw;
      }
      normalized.resize(normalization_indices.size());
      for (auto &result : normalized) {
        result.failure = PhotoNormalizationFailure::NormalizationFailed;
      }
    }
    if (normalized.size() != normalization_indices.size()) {
      throw MediaFallbackError("photo_normalization", "incomplete_outcomes",
                               media.size(), replaced_indices.size(),
                               normalized_count);
    }
    for (std::size_t index = 0; index < normalized.size(); ++index) {
      const auto outcome_index = normalization_indices[index];
      if (!normalized[index].succeeded()) {
        outcomes[outcome_index].failure =
            media_failure(normalized[index].failure);
        outcomes[outcome_index].diagnostic =
            std::string{failure_name(normalized[index].failure)};
        OBCX_WARN("QQ multipart 图片尺寸处理失败: index={}, category={}",
                  outcome_index + 1, failure_name(normalized[index].failure));
        continue;
      }
      if (normalized[index].normalized) {
        ++normalized_count;
        OBCX_INFO("QQ multipart 图片尺寸已调整: index={}, {}x{} -> {}x{}",
                  outcome_index + 1, normalized[index].source_dimensions.width,
                  normalized[index].source_dimensions.height,
                  normalized[index].output_dimensions.width,
                  normalized[index].output_dimensions.height);
      }
      outcomes[outcome_index].image = std::move(normalized[index].image);
    }
  }

  const bool needs_placeholder = std::ranges::any_of(
      outcomes, [](const auto &outcome) { return !outcome.succeeded(); });

  DownloadedImage placeholder = embedded_placeholder();
  if (needs_placeholder && !config_->image_placeholder_url.empty()) {
    try {
      const std::vector<std::pair<std::string, std::string>> placeholder_media =
          {{"photo", config_->image_placeholder_url}};
      auto placeholder_outcomes =
          co_await image_downloader_(*config_, placeholder_media);
      if (placeholder_outcomes.size() == 1 &&
          placeholder_outcomes.front().succeeded()) {
        std::vector<DownloadedImage> placeholder_candidate;
        placeholder_candidate.push_back(
            std::move(*placeholder_outcomes.front().image));
        auto normalized_placeholder = co_await blocking_executor_->run(
            [normalizer = image_normalizer_, config = config_,
             candidate = std::move(placeholder_candidate)]() mutable {
              return normalizer(*config, std::move(candidate));
            });
        if (normalized_placeholder.size() == 1 &&
            normalized_placeholder.front().succeeded()) {
          placeholder = std::move(*normalized_placeholder.front().image);
        } else {
          OBCX_WARN("配置占位图尺寸处理失败，使用内置占位图: "
                    "category=invalid_placeholder");
        }
      } else {
        const auto category =
            placeholder_outcomes.size() == 1
                ? failure_name(placeholder_outcomes.front().failure)
                : std::string_view{"incomplete_outcomes"};
        OBCX_WARN("配置占位图下载失败，使用内置占位图: category={}", category);
      }
    } catch (const std::exception &error) {
      if (is_operation_aborted(error)) {
        throw;
      }
      OBCX_WARN("配置占位图下载失败，使用内置占位图: category={}",
                telegram_failure_category(error.what()));
    }
  }

  std::vector<DownloadedImage> downloaded;
  downloaded.reserve(media.size());
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    if (outcomes[index].succeeded()) {
      downloaded.push_back(std::move(*outcomes[index].image));
      continue;
    }

    replaced_indices.insert(media[index].original_index);
    auto replacement = placeholder;
    replacement.type = media[index].type.empty() ? "photo" : media[index].type;
    replacement.original_url.clear();
    replacement.filename = "qq-media-" + std::to_string(index) + ".png";
    downloaded.push_back(std::move(replacement));
    OBCX_WARN("QQ multipart 媒体已替换: index={}, category={}, limit={}",
              index + 1, failure_name(outcomes[index].failure),
              config_->qq_media_download_max_bytes);
  }

  TemporaryMediaUploads temporary;
  try {
    temporary = co_await blocking_executor_->run(
        [downloaded = std::move(downloaded)]() mutable {
          return TemporaryMediaUploads::materialize(std::move(downloaded));
        });
  } catch (const std::exception &error) {
    if (is_operation_aborted(error)) {
      throw;
    }
    throw MediaFallbackError("temporary_media", "materialize_failed",
                             media.size(), replaced_indices.size(),
                             normalized_count);
  }
  const auto temporary_root = temporary.root();

  std::optional<obcx::bot::SendMessageResult> response;
  std::string upload_failure_category;
  std::exception_ptr upload_cancellation;
  try {
    const auto maximum_bytes = std::min(obcx::bot::maximum_actor_media_bytes,
                                        config_->qq_media_download_max_bytes *
                                            temporary.uploads().size());
    response = co_await operations_->send_telegram_media_uploads(
        telegram_group_id, temporary.uploads(),
        caption_with_media_recovery(caption, replaced_indices.size(),
                                    normalized_count),
        maximum_bytes, topic_id, reply_to_message_id, caption_entities);
  } catch (const std::exception &error) {
    if (is_operation_aborted(error)) {
      upload_cancellation = std::current_exception();
    } else {
      upload_failure_category = telegram_failure_category(error.what());
    }
  } catch (...) {
    upload_failure_category = "unknown_exception";
  }

  std::string cleanup_error;
  std::exception_ptr cleanup_cancellation;
  try {
    cleanup_error = co_await blocking_executor_->run([temporary_root] {
      std::error_code error;
      fs::remove_all(temporary_root, error);
      return error ? error.message() : std::string{};
    });
  } catch (const std::exception &error) {
    if (is_operation_aborted(error)) {
      cleanup_cancellation = std::current_exception();
    } else {
      cleanup_error = "cleanup_operation_failed";
    }
  }
  if (cleanup_error.empty()) {
    temporary.mark_cleaned();
    OBCX_TRACE("已删除 QQ MediaGroup 临时目录: {}", temporary_root.string());
  } else {
    OBCX_WARN("删除 QQ MediaGroup 临时目录失败，将在析构时重试: {} ({})",
              temporary_root.string(), cleanup_error);
  }

  if (cleanup_cancellation) {
    std::rethrow_exception(cleanup_cancellation);
  }
  if (upload_cancellation) {
    std::rethrow_exception(upload_cancellation);
  }
  if (!upload_failure_category.empty()) {
    throw MediaFallbackError("multipart_upload", upload_failure_category,
                             media.size(), replaced_indices.size(),
                             normalized_count);
  }
  co_return MediaGroupFallbackResult{
      .send_result = std::move(response),
      .used_multipart = true,
      .replaced_count = replaced_indices.size(),
      .normalized_count = normalized_count,
  };
}

auto QQMessageFormatter::format_sender_info(
    const obcx::common::MessageEvent &event,
    const GroupBridgeConfig *bridge_config, const std::string &qq_group_id,
    const std::string &telegram_group_id, int64_t topic_id,
    obcx::common::Message &message_to_send)
    -> boost::asio::awaitable<std::string> {

  std::string sender_display_name = co_await get_user_display_name(
      event.user_id, event.group_id.value_or(""));

  bool show_sender = false;
  if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
    show_sender = bridge_config->show_qq_to_tg_sender;
  } else {
    const TopicBridgeConfig *topic_config = config_->topic_config(
        operations_->pair_id(), telegram_group_id, topic_id);
    show_sender = topic_config ? topic_config->show_qq_to_tg_sender : false;
  }

  if (show_sender) {
    append_italic_telegram_sender(message_to_send, sender_display_name);
    OBCX_DEBUG("QQ到Telegram转发显示发送者：{}", sender_display_name);
  } else {
    OBCX_DEBUG("QQ到Telegram转发不显示发送者");
  }

  co_return sender_display_name;
}

auto QQMessageFormatter::format_reply_message(
    const obcx::common::MessageEvent &event,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<bool> {

  std::optional<std::string> reply_message_id;
  for (const auto &segment : event.message) {
    if (segment.type == "reply") {
      reply_message_id = obcx::common::JsonUtils::get_optional_id_as_string(
          segment.data, "id");
      if (reply_message_id.has_value()) {
        OBCX_DEBUG("检测到QQ引用消息，引用ID: {}", reply_message_id.value());
        break;
      }
    }
  }

  if (reply_message_id.has_value()) {
    std::optional<std::string> target_telegram_message_id;

    // 被回复的 QQ 消息可能有两种来源：原生 QQ 消息（之前转发到 TG 过），
    // 或本身是从 TG 转发来的。先查 qq->tg 映射，没命中再查 tg 原始消息。
    if (state_repository_) {
      const auto onebot_installation =
          operations_->onebot11_installation().installation_id;
      const auto telegram_installation =
          operations_->telegram_installation().installation_id;
      const auto qq_group_id = event.group_id.value_or("");
      const auto [telegram_group_id, topic_id] =
          config_->tg_group_and_topic_id(operations_->pair_id(), qq_group_id);
      (void)topic_id;
      if (!qq_group_id.empty() && !telegram_group_id.empty()) {
        const auto source_conversation = qq_conversation_id(qq_group_id);
        const auto target_conversation =
            telegram_conversation_id(telegram_group_id);
        target_telegram_message_id = co_await blocking_executor_->run(
            [repository = state_repository_,
             received = received_message_repository_, onebot_installation,
             telegram_installation, source_conversation, target_conversation,
             reply_message_id = *reply_message_id] {
              const auto direct = repository->resolve_target_mapping(
                  {.installation_id = onebot_installation,
                   .platform = "qq",
                   .conversation_id = source_conversation,
                   .message_id = reply_message_id},
                  {.installation_id = telegram_installation,
                   .platform = "telegram",
                   .conversation_id = target_conversation});
              if (direct.unique()) {
                if (received && !received->get_message(
                                    "qq", onebot_installation,
                                    source_conversation, reply_message_id)) {
                  return std::optional<std::string>{};
                }
                return std::optional<std::string>{
                    direct.mapping->target_message_id};
              }
              if (!direct.missing()) {
                throw std::runtime_error(direct.diagnostic.empty()
                                             ? "ambiguous_message_mapping"
                                             : direct.diagnostic);
              }
              const auto reverse = repository->resolve_source_mapping(
                  {.installation_id = onebot_installation,
                   .platform = "qq",
                   .conversation_id = source_conversation,
                   .message_id = reply_message_id},
                  {.installation_id = telegram_installation,
                   .platform = "telegram",
                   .conversation_id = target_conversation});
              if (reverse.unique()) {
                if (received &&
                    !received->get_message(
                        "telegram", telegram_installation, target_conversation,
                        reverse.mapping->source_message_id)) {
                  return std::optional<std::string>{};
                }
                return std::optional<std::string>{
                    reverse.mapping->source_message_id};
              }
              if (!reverse.missing()) {
                throw std::runtime_error(reverse.diagnostic.empty()
                                             ? "ambiguous_message_mapping"
                                             : reverse.diagnostic);
              }
              return std::optional<std::string>{};
            });
      }
    }

    OBCX_DEBUG("QQ回复消息映射查找: QQ消息ID {} -> TG消息ID {}",
               reply_message_id.value(),
               target_telegram_message_id.has_value()
                   ? target_telegram_message_id.value()
                   : "未找到");

    if (target_telegram_message_id.has_value()) {
      obcx::common::MessageSegment reply_segment;
      reply_segment.type = "reply";
      reply_segment.data["id"] = target_telegram_message_id.value();
      message_to_send.push_back(reply_segment);
      OBCX_DEBUG("添加Telegram引用消息段，引用ID: {}",
                 target_telegram_message_id.value());
      co_return true;
    } else {
      OBCX_DEBUG("未找到QQ引用消息对应的Telegram消息ID，可能是原生QQ消息");
    }
  }

  co_return false;
}

auto QQMessageFormatter::process_forward_message(
    const obcx::common::MessageSegment &segment,
    const std::string &telegram_group_id, int64_t topic_id,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<void> {

  try {
    std::string forward_id = segment.data.value("id", "");
    if (forward_id.empty()) {
      co_return;
    }

    OBCX_DEBUG("处理合并转发消息，ID: {}", forward_id);

    nlohmann::json forward_data{
        {"messages",
         co_await operations_->get_onebot11_forward_messages(forward_id)}};

    obcx::common::MessageSegment forward_title_segment;
    forward_title_segment.type = "text";
    forward_title_segment.data["text"] = "\n📋 合并转发消息:\n";
    message_to_send.push_back(forward_title_segment);

    // 合并转发节点里的图片要单独走 sendMediaGroup，不能塞进文本里。
    std::vector<obcx::common::MessageSegment> forward_images;

    if (forward_data.contains("messages") &&
        forward_data["messages"].is_array()) {
      for (const auto &msg_node : forward_data["messages"]) {
        if (msg_node.is_object()) {
          std::string node_sender =
              msg_node.value("sender", nlohmann::json::object())
                  .value("nickname", "未知用户");

          std::string node_content = "";
          if (msg_node.contains("content") && msg_node["content"].is_array()) {
            for (const auto &content_seg : msg_node["content"]) {
              if (content_seg.is_object() && content_seg.contains("type")) {
                std::string seg_type = content_seg["type"];
                if (seg_type == "text" && content_seg.contains("data") &&
                    content_seg["data"].contains("text")) {
                  node_content +=
                      content_seg["data"]["text"].get<std::string>();
                } else if (seg_type == "face" && content_seg.contains("data") &&
                           content_seg["data"].contains("id")) {
                  node_content +=
                      fmt::format("[表情:{}]",
                                  content_seg["data"]["id"].get<std::string>());
                } else if (seg_type == "image") {
                  // 占位文字：图片本体在收集后批量发送，节点文本里只保留序号引用。
                  node_content +=
                      fmt::format("[图片{}]", forward_images.size() + 1);

                  obcx::common::MessageSegment img_segment;
                  img_segment.type = "image";
                  if (content_seg.contains("data")) {
                    auto img_data = content_seg["data"];
                    if (img_data.contains("url") &&
                        img_data["url"].is_string()) {
                      img_segment.data["url"] =
                          img_data["url"].get<std::string>();
                      img_segment.data["file"] =
                          img_data["url"].get<std::string>();
                    } else if (img_data.contains("file") &&
                               img_data["file"].is_string()) {
                      img_segment.data["file"] =
                          img_data["file"].get<std::string>();
                    }
                    if (img_data.contains("subType")) {
                      img_segment.data["subType"] = img_data["subType"];
                    }
                  }
                  forward_images.push_back(img_segment);
                  OBCX_DEBUG(
                      "收集合并转发中的图片: url={}",
                      img_segment.data.value(
                          "url", img_segment.data.value("file", "无URL")));
                } else if (seg_type == "at" && content_seg.contains("data") &&
                           content_seg["data"].contains("qq")) {
                  node_content += fmt::format(
                      "[@{}]", content_seg["data"]["qq"].get<std::string>());
                } else {
                  node_content += fmt::format("[{}]", seg_type);
                }
              }
            }
          } else if (msg_node.contains("content") &&
                     msg_node["content"].is_string()) {
            // 兼容老版本 content 直接是字符串的格式
            node_content = msg_node["content"].get<std::string>();
          }

          obcx::common::MessageSegment node_segment;
          node_segment.type = "text";
          node_segment.data["text"] =
              fmt::format("👤 {}: {}\n", node_sender, node_content);
          message_to_send.push_back(node_segment);
        }
      }
    }

    // 合并转发收集到的图片同样走 sendMediaGroup（每批最多10张）。
    if (!forward_images.empty()) {
      OBCX_INFO("合并转发消息中发现 {} 张图片，准备使用MediaGroup发送",
                forward_images.size());

      std::vector<std::pair<std::string, std::string>> all_media;
      for (const auto &img_seg : forward_images) {
        std::string url =
            img_seg.data.value("url", img_seg.data.value("file", ""));
        if (!url.empty()) {
          all_media.emplace_back("photo", url);
          OBCX_DEBUG("添加图片到MediaGroup: {}", url);
        }
      }

      if (!all_media.empty()) {
        std::optional<int64_t> opt_topic_id =
            (topic_id == -1) ? std::nullopt : std::optional<int64_t>(topic_id);
        size_t total_batches = (all_media.size() + 9) / 10;
        size_t sent_count = 0;
        size_t total_replaced_count = 0;
        size_t total_normalized_count = 0;

        size_t batch_start = 0;
        for (size_t batch = 0; batch < total_batches; ++batch) {
          const size_t remaining = all_media.size() - batch_start;
          // Telegram media groups require at least two items. When eleven
          // remain, send 9 + 2 instead of 10 + 1.
          const size_t batch_size =
              remaining == 11 ? 9
                              : std::min(static_cast<size_t>(10), remaining);

          std::vector<std::pair<std::string, std::string>> batch_media(
              all_media.begin() + batch_start,
              all_media.begin() + batch_start + batch_size);
          const auto original_batch_media = batch_media;

          std::vector<std::string> replaced;
          batch_media =
              co_await image_sanitizer_(*config_, batch_media, replaced);

          std::vector<PreparedMedia> prepared;
          prepared.reserve(batch_media.size());
          for (std::size_t index = 0; index < batch_media.size(); ++index) {
            prepared.push_back(PreparedMedia{
                .type = batch_media[index].first,
                .url = batch_media[index].second,
                .original_index = batch_start + index,
                .replaced = batch_media[index].second !=
                            original_batch_media[index].second,
            });
          }

          try {
            const std::string caption = fmt::format(
                "📸 合并转发消息中的图片 ({}/{})", batch + 1, total_batches);
            const auto send_result = co_await send_media_group_with_fallback(
                telegram_group_id, prepared, caption, opt_topic_id,
                std::nullopt);

            OBCX_INFO("成功通过MediaGroup发送第 {}/{} 批 {} 张图片{}",
                      batch + 1, total_batches, batch_media.size(),
                      send_result.used_multipart ? "（multipart兜底）" : "");
            sent_count += batch_media.size();
            total_replaced_count += send_result.replaced_count;
            total_normalized_count += send_result.normalized_count;
          } catch (const MediaFallbackError &error) {
            OBCX_ERROR("合并转发 MediaGroup 第 {}/{} 批整体发送失败: "
                       "stage={}, category={}, normalized={}, replaced={}/{}",
                       batch + 1, total_batches, error.stage(),
                       error.category(), error.normalized_count(),
                       error.replaced_count(), error.item_count());
            obcx::common::MessageSegment error_segment;
            error_segment.type = "text";
            error_segment.data["text"] = fmt::format(
                "\n[第{}/{}批（{}张）整体发送失败：阶段={}，原因={}，"
                "已调整={}，已替换={}/{}]",
                batch + 1, total_batches, batch_media.size(), error.stage(),
                error.category(), error.normalized_count(),
                error.replaced_count(), error.item_count());
            message_to_send.push_back(error_segment);
          } catch (const std::exception &) {
            OBCX_ERROR("合并转发 MediaGroup 第 {}/{} 批整体发送失败: "
                       "stage=unexpected, category=unclassified",
                       batch + 1, total_batches);
            obcx::common::MessageSegment error_segment;
            error_segment.type = "text";
            error_segment.data["text"] = fmt::format(
                "\n[第{}/{}批（{}张）整体发送失败：阶段=unexpected，"
                "原因=unclassified]",
                batch + 1, total_batches, batch_media.size());
            message_to_send.push_back(error_segment);
          }
          batch_start += batch_size;
        }

        if (sent_count > 0) {
          OBCX_INFO("合并转发消息图片发送完成，共成功发送 {}/{} 张 "
                    "(已调整 {} 张，已用占位图替换 {} 张)",
                    sent_count, all_media.size(), total_normalized_count,
                    total_replaced_count);
        }
      }
    }

    OBCX_INFO("成功处理合并转发消息，包含 {} 条消息，{} 张图片",
              forward_data.value("messages", nlohmann::json::array()).size(),
              forward_images.size());
  } catch (const std::exception &e) {
    OBCX_ERROR("处理合并转发消息时出错: {}", e.what());
    obcx::common::MessageSegment error_segment;
    error_segment.type = "text";
    error_segment.data["text"] = "[合并转发消息处理失败]";
    message_to_send.push_back(error_segment);
  }
}

auto QQMessageFormatter::process_node_message(
    const obcx::common::MessageSegment &segment,
    obcx::common::Message &message_to_send) -> boost::asio::awaitable<void> {

  try {
    std::string node_user_id = segment.data.value("user_id", "");
    std::string node_nickname = segment.data.value("nickname", "未知用户");

    if (segment.data.contains("content")) {
      auto content = segment.data.at("content");

      obcx::common::MessageSegment node_segment;
      node_segment.type = "text";

      if (content.is_string()) {
        node_segment.data["text"] = fmt::format("👤 {}: {}\n", node_nickname,
                                                content.get<std::string>());
      } else if (content.is_array()) {
        std::string node_text = fmt::format("👤 {}: ", node_nickname);
        for (const auto &content_segment : content) {
          if (content_segment.is_object() && content_segment.contains("type")) {
            std::string seg_type = content_segment["type"];
            if (seg_type == "text" && content_segment.contains("data") &&
                content_segment["data"].contains("text")) {
              node_text += content_segment["data"]["text"].get<std::string>();
            } else if (seg_type == "face") {
              node_text += fmt::format(
                  "[表情:{}]",
                  content_segment.value("data", nlohmann::json::object())
                      .value("id", "0"));
            } else if (seg_type == "image") {
              node_text += "[图片]";
            } else {
              node_text += fmt::format("[{}]", seg_type);
            }
          }
        }
        node_text += "\n";
        node_segment.data["text"] = node_text;
      } else {
        node_segment.data["text"] =
            fmt::format("👤 {}: [未知内容]\n", node_nickname);
      }

      message_to_send.push_back(node_segment);
      OBCX_DEBUG("处理node消息段: 用户 {} ({})", node_nickname, node_user_id);
    }
  } catch (const std::exception &e) {
    OBCX_ERROR("处理node消息段时出错: {}", e.what());
    obcx::common::MessageSegment error_segment;
    error_segment.type = "text";
    error_segment.data["text"] = "[转发节点处理失败]";
    message_to_send.push_back(error_segment);
  }

  co_return;
}

auto QQMessageFormatter::send_media_group(
    const std::vector<obcx::common::MessageSegment> &image_segments,
    const std::vector<obcx::common::MessageSegment> &other_segments,
    const std::string &telegram_group_id, int64_t topic_id,
    const std::string &sender_display_name,
    const GroupBridgeConfig *bridge_config,
    const obcx::common::Message &message_to_send,
    const obcx::common::MessageEvent &event)
    -> boost::asio::awaitable<MediaGroupSendResult> {

  MediaGroupSendResult result;

  if (image_segments.size() <= 1) {
    co_return result;
  }

  OBCX_INFO("检测到多张图片({})，使用MediaGroup发送", image_segments.size());

  for (size_t sent_count = 0; sent_count < image_segments.size();) {
    const size_t remaining = image_segments.size() - sent_count;
    // Keep a trailing item paired: 11 becomes 9 + 2, never 10 + 1.
    const size_t batch_size =
        remaining == 11 ? 9 : std::min(static_cast<size_t>(10), remaining);
    OBCX_DEBUG("准备发送MediaGroup图片，起始索引: {}, 本批次数量: {}",
               sent_count, batch_size);
    std::vector<std::pair<std::string, std::string>> media_list;
    for (size_t i = 0; i < batch_size; ++i) {
      std::string url = image_segments[sent_count + i].data.value(
          "url", image_segments[sent_count + i].data.value("file", ""));
      if (!url.empty()) {
        media_list.emplace_back("photo", url);
        OBCX_DEBUG("添加图片到MediaGroup: {}", url);
      }
    }

    const auto original_media_list = media_list;
    std::vector<std::string> replaced;
    if (!media_list.empty()) {
      media_list = co_await image_sanitizer_(*config_, media_list, replaced);
    }

    std::vector<PreparedMedia> prepared;
    prepared.reserve(media_list.size());
    for (std::size_t index = 0; index < media_list.size(); ++index) {
      prepared.push_back(PreparedMedia{
          .type = media_list[index].first,
          .url = media_list[index].second,
          .original_index = sent_count + index,
          .replaced =
              media_list[index].second != original_media_list[index].second,
      });
    }

    if (!media_list.empty()) {
      try {
        std::string caption;
        std::vector<obcx::bot::TelegramTextEntity> caption_entities;

        bool show_sender = false;
        if (bridge_config->mode == BridgeMode::GROUP_TO_GROUP) {
          show_sender = bridge_config->show_qq_to_tg_sender;
        } else {
          const TopicBridgeConfig *topic_config = config_->topic_config(
              operations_->pair_id(), telegram_group_id, topic_id);
          show_sender =
              topic_config ? topic_config->show_qq_to_tg_sender : false;
        }

        if (show_sender) {
          caption = fmt::format("[{}]", sender_display_name);
          caption_entities = italic_entity(caption);
        }

        for (const auto &seg : other_segments) {
          if (seg.type == "text" && seg.data.contains("text")) {
            if (!caption.empty()) {
              caption += "\n";
            }
            caption += seg.data.at("text");
          }
        }

        std::optional<int64_t> opt_topic_id =
            (topic_id == -1) ? std::nullopt : std::optional<int64_t>(topic_id);

        std::optional<std::string> opt_reply_id;
        for (const auto &seg : message_to_send) {
          if (seg.type == "reply") {
            opt_reply_id = obcx::common::JsonUtils::get_optional_id_as_string(
                seg.data, "id");
            if (opt_reply_id.has_value()) {
              break;
            }
          }
        }

        const auto send_result = co_await send_media_group_with_fallback(
            telegram_group_id, prepared, caption, opt_topic_id, opt_reply_id,
            caption_entities);

        OBCX_INFO("成功通过MediaGroup发送 {} 张图片{}，已调整 {} 张，"
                  "已替换 {} 张",
                  media_list.size(),
                  send_result.used_multipart ? "（multipart兜底）" : "",
                  send_result.normalized_count, send_result.replaced_count);

        if (send_result.send_result.has_value() &&
            !send_result.send_result->messages.empty()) {
          const auto &target = send_result.send_result->messages.front();
          if (!result.primary_target_message_id.has_value()) {
            result.primary_target_message_id = target.native_message_id;
          }
          OBCX_DEBUG("MediaGroup主消息: QQ {} -> TG {}", event.message_id,
                     target.native_message_id);
        }

        result.sent = true;
      } catch (const MediaFallbackError &error) {
        OBCX_ERROR("MediaGroup 整体发送失败: stage={}, category={}, "
                   "normalized={}, replaced={}/{}",
                   error.stage(), error.category(), error.normalized_count(),
                   error.replaced_count(), error.item_count());
      } catch (const std::exception &) {
        OBCX_ERROR("MediaGroup 整体发送失败: stage=unexpected, "
                   "category=unclassified");
      }
    }
    sent_count += batch_size;
  }
  co_return result;
}

auto QQMessageFormatter::get_user_display_name(const std::string &user_id,
                                               const std::string &group_id)
    -> boost::asio::awaitable<std::string> {

  std::optional<std::string> display_name;
  if (state_repository_) {
    const auto installation =
        operations_->onebot11_installation().installation_id;
    display_name = co_await blocking_executor_->run(
        [repository = state_repository_, installation, user_id, group_id] {
          return repository->query_user_display_name(installation, "qq",
                                                     user_id, group_id);
        });
  }

  if (!display_name.has_value()) {
    co_await fetch_user_info(user_id, group_id);
    if (state_repository_) {
      const auto installation =
          operations_->onebot11_installation().installation_id;
      display_name = co_await blocking_executor_->run(
          [repository = state_repository_, installation, user_id, group_id] {
            return repository->query_user_display_name(installation, "qq",
                                                       user_id, group_id);
          });
    }
  }

  co_return display_name.value_or(user_id);
}

auto QQMessageFormatter::fetch_user_info(const std::string &user_id,
                                         const std::string &group_id)
    -> boost::asio::awaitable<void> {
  co_await fetch_and_save_user_info(operations_, state_repository_,
                                    blocking_executor_, user_id, group_id);
}

auto QQMessageFormatter::fetch_and_save_user_info(
    std::shared_ptr<BridgeBotOperations> operations,
    std::shared_ptr<bridge::BridgeStateRepository> state_repository,
    std::shared_ptr<obcx::core::BlockingExecutor> blocking_executor,
    const std::string &user_id, const std::string &group_id)
    -> boost::asio::awaitable<void> {
  try {
    const auto member =
        co_await operations->get_onebot11_group_member(group_id, user_id);
    storage::UserInfo user_info;
    user_info.installation_id =
        operations->onebot11_installation().installation_id;
    user_info.platform = "qq";
    user_info.user_id = user_id;
    user_info.group_id = group_id;
    user_info.nickname = !member.card.empty()    ? member.card
                         : !member.title.empty() ? member.title
                                                 : member.nickname;
    user_info.title = member.title;
    user_info.last_updated = std::chrono::system_clock::now();
    if (state_repository) {
      (void)co_await blocking_executor->run([state_repository, user_info] {
        return state_repository->save_or_update_user(user_info, true);
      });
    }
  } catch (const std::exception &error) {
    OBCX_DEBUG("获取QQ用户信息失败：{}", error.what());
  }
}

} // namespace bridge::qq

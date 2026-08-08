#include "qq/photo_normalizer.hpp"

#include "media_processor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <poll.h>
#include <span>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace bridge::qq {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kDiagnosticTailLimit = 4096;
constexpr std::uint64_t kGeometryScaleSum = 9990;

class TemporaryPhotoDirectory {
public:
  TemporaryPhotoDirectory() {
    static std::atomic_uint64_t sequence{0};
    std::error_code error;
    root_ = fs::temp_directory_path(error) /
            fmt::format("obcx-qq-photo-{}-{}", ::getpid(),
                        sequence.fetch_add(1, std::memory_order_relaxed));
    if (error || !fs::create_directory(root_, error)) {
      root_.clear();
    }
  }

  TemporaryPhotoDirectory(const TemporaryPhotoDirectory &) = delete;
  auto operator=(const TemporaryPhotoDirectory &)
      -> TemporaryPhotoDirectory & = delete;

  ~TemporaryPhotoDirectory() {
    if (root_.empty()) {
      return;
    }
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  [[nodiscard]] auto root() const -> const fs::path & { return root_; }
  [[nodiscard]] auto valid() const -> bool { return !root_.empty(); }

private:
  fs::path root_;
};

using Bytes = std::span<const std::uint8_t>;

[[nodiscard]] auto bytes(std::string_view encoded) -> Bytes {
  return {reinterpret_cast<const std::uint8_t *>(encoded.data()),
          encoded.size()};
}

[[nodiscard]] auto read_be16(Bytes data, std::size_t offset)
    -> std::optional<std::uint16_t> {
  if (offset > data.size() || data.size() - offset < 2) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(data[offset]) << 8U) | data[offset + 1]);
}

[[nodiscard]] auto read_le16(Bytes data, std::size_t offset)
    -> std::optional<std::uint16_t> {
  if (offset > data.size() || data.size() - offset < 2) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(
      data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8U));
}

[[nodiscard]] auto read_be32(Bytes data, std::size_t offset)
    -> std::optional<std::uint32_t> {
  if (offset > data.size() || data.size() - offset < 4) {
    return std::nullopt;
  }
  return (static_cast<std::uint32_t>(data[offset]) << 24U) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
         data[offset + 3];
}

[[nodiscard]] auto read_le32(Bytes data, std::size_t offset)
    -> std::optional<std::uint32_t> {
  if (offset > data.size() || data.size() - offset < 4) {
    return std::nullopt;
  }
  return data[offset] | (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

[[nodiscard]] auto valid_dimensions(std::uint64_t width, std::uint64_t height)
    -> std::optional<PhotoDimensions> {
  if (width == 0 || height == 0 ||
      width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return PhotoDimensions{.width = static_cast<std::uint32_t>(width),
                         .height = static_cast<std::uint32_t>(height)};
}

[[nodiscard]] auto jpeg_dimensions(Bytes data)
    -> std::optional<PhotoDimensions> {
  if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8) {
    return std::nullopt;
  }

  std::size_t cursor = 2;
  while (cursor < data.size()) {
    while (cursor < data.size() && data[cursor] != 0xff) {
      ++cursor;
    }
    if (cursor == data.size()) {
      break;
    }
    while (cursor < data.size() && data[cursor] == 0xff) {
      ++cursor;
    }
    if (cursor == data.size()) {
      break;
    }

    const auto marker = data[cursor++];
    if (marker == 0x00 || marker == 0x01 || marker == 0xd8 ||
        (marker >= 0xd0 && marker <= 0xd7)) {
      continue;
    }
    if (marker == 0xd9 || marker == 0xda) {
      break;
    }

    const auto length = read_be16(data, cursor);
    if (!length || *length < 2 || cursor > data.size() ||
        static_cast<std::size_t>(*length) > data.size() - cursor) {
      return std::nullopt;
    }

    const bool start_of_frame = (marker >= 0xc0 && marker <= 0xc3) ||
                                (marker >= 0xc5 && marker <= 0xc7) ||
                                (marker >= 0xc9 && marker <= 0xcb) ||
                                (marker >= 0xcd && marker <= 0xcf);
    if (start_of_frame) {
      if (*length < 7) {
        return std::nullopt;
      }
      const auto height = read_be16(data, cursor + 3);
      const auto width = read_be16(data, cursor + 5);
      if (!width || !height) {
        return std::nullopt;
      }
      return valid_dimensions(*width, *height);
    }
    cursor += *length;
  }
  return std::nullopt;
}

[[nodiscard]] auto png_dimensions(Bytes data)
    -> std::optional<PhotoDimensions> {
  static constexpr std::array<std::uint8_t, 8> signature = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
  const auto ihdr_length = read_be32(data, 8);
  if (data.size() < 24 || !ihdr_length || *ihdr_length != 13 ||
      !std::equal(signature.begin(), signature.end(), data.begin()) ||
      std::string_view{reinterpret_cast<const char *>(data.data() + 12), 4} !=
          "IHDR") {
    return std::nullopt;
  }
  const auto width = read_be32(data, 16);
  const auto height = read_be32(data, 20);
  return width && height ? valid_dimensions(*width, *height) : std::nullopt;
}

[[nodiscard]] auto gif_dimensions(Bytes data)
    -> std::optional<PhotoDimensions> {
  if (data.size() < 10 ||
      (std::string_view{reinterpret_cast<const char *>(data.data()), 6} !=
           "GIF87a" &&
       std::string_view{reinterpret_cast<const char *>(data.data()), 6} !=
           "GIF89a")) {
    return std::nullopt;
  }
  const auto width = read_le16(data, 6);
  const auto height = read_le16(data, 8);
  return width && height ? valid_dimensions(*width, *height) : std::nullopt;
}

[[nodiscard]] auto webp_dimensions(Bytes data)
    -> std::optional<PhotoDimensions> {
  if (data.size() < 20 ||
      std::string_view{reinterpret_cast<const char *>(data.data()), 4} !=
          "RIFF" ||
      std::string_view{reinterpret_cast<const char *>(data.data() + 8), 4} !=
          "WEBP") {
    return std::nullopt;
  }

  const auto chunk =
      std::string_view{reinterpret_cast<const char *>(data.data() + 12), 4};
  const auto chunk_size = read_le32(data, 16);
  if (!chunk_size) {
    return std::nullopt;
  }
  if (chunk == "VP8X") {
    if (*chunk_size < 10 || data.size() < 30) {
      return std::nullopt;
    }
    const std::uint32_t width = 1U + data[24] +
                                (static_cast<std::uint32_t>(data[25]) << 8U) +
                                (static_cast<std::uint32_t>(data[26]) << 16U);
    const std::uint32_t height = 1U + data[27] +
                                 (static_cast<std::uint32_t>(data[28]) << 8U) +
                                 (static_cast<std::uint32_t>(data[29]) << 16U);
    return valid_dimensions(width, height);
  }
  if (chunk == "VP8 ") {
    if (*chunk_size < 10 || data.size() < 30 || data[23] != 0x9d ||
        data[24] != 0x01 || data[25] != 0x2a) {
      return std::nullopt;
    }
    const auto raw_width = read_le16(data, 26);
    const auto raw_height = read_le16(data, 28);
    if (!raw_width || !raw_height) {
      return std::nullopt;
    }
    return valid_dimensions(*raw_width & 0x3fffU, *raw_height & 0x3fffU);
  }
  if (chunk == "VP8L") {
    if (*chunk_size < 5 || data.size() < 25 || data[20] != 0x2f) {
      return std::nullopt;
    }
    const std::uint32_t width =
        1U + data[21] + ((static_cast<std::uint32_t>(data[22]) & 0x3fU) << 8U);
    const std::uint32_t height =
        1U + ((static_cast<std::uint32_t>(data[22]) & 0xc0U) >> 6U) +
        (static_cast<std::uint32_t>(data[23]) << 2U) +
        ((static_cast<std::uint32_t>(data[24]) & 0x0fU) << 10U);
    return valid_dimensions(width, height);
  }
  return std::nullopt;
}

[[nodiscard]] auto bmp_dimensions(Bytes data)
    -> std::optional<PhotoDimensions> {
  if (data.size() < 26 || data[0] != 'B' || data[1] != 'M') {
    return std::nullopt;
  }
  const auto dib_size = read_le32(data, 14);
  if (!dib_size) {
    return std::nullopt;
  }
  if (*dib_size == 12) {
    const auto width = read_le16(data, 18);
    const auto height = read_le16(data, 20);
    return width && height ? valid_dimensions(*width, *height) : std::nullopt;
  }
  if (*dib_size < 40) {
    return std::nullopt;
  }
  const auto raw_width = read_le32(data, 18);
  const auto raw_height = read_le32(data, 22);
  if (!raw_width || !raw_height) {
    return std::nullopt;
  }
  const auto width = static_cast<std::int32_t>(*raw_width);
  const auto height = static_cast<std::int32_t>(*raw_height);
  if (width <= 0 || height == 0 ||
      height == std::numeric_limits<std::int32_t>::min()) {
    return std::nullopt;
  }
  const auto absolute_height = height < 0 ? static_cast<std::uint32_t>(-height)
                                          : static_cast<std::uint32_t>(height);
  return valid_dimensions(static_cast<std::uint32_t>(width), absolute_height);
}

[[nodiscard]] auto encoded_dimensions(std::string_view encoded)
    -> std::optional<PhotoDimensions> {
  const auto data = bytes(encoded);
  if (auto dimensions = jpeg_dimensions(data)) {
    return dimensions;
  }
  if (auto dimensions = png_dimensions(data)) {
    return dimensions;
  }
  if (auto dimensions = gif_dimensions(data)) {
    return dimensions;
  }
  if (auto dimensions = webp_dimensions(data)) {
    return dimensions;
  }
  return bmp_dimensions(data);
}

[[nodiscard]] auto ceil_div(std::uint64_t value, std::uint64_t divisor)
    -> std::uint64_t {
  return value / divisor + (value % divisor == 0 ? 0 : 1);
}

void append_diagnostic_tail(std::string &target, const char *data,
                            std::size_t size) {
  if (size >= kDiagnosticTailLimit) {
    target.assign(data + size - kDiagnosticTailLimit, kDiagnosticTailLimit);
    return;
  }
  target.append(data, size);
  if (target.size() > kDiagnosticTailLimit) {
    target.erase(0, target.size() - kDiagnosticTailLimit);
  }
}

[[nodiscard]] auto input_extension(std::string_view mime) -> std::string_view {
  if (mime == "image/png") {
    return ".png";
  }
  if (mime == "image/gif") {
    return ".gif";
  }
  if (mime == "image/webp") {
    return ".webp";
  }
  if (mime == "image/bmp") {
    return ".bmp";
  }
  return ".jpg";
}

[[nodiscard]] auto failed_normalization(PhotoNormalizationFailure failure,
                                        PhotoDimensions source = {})
    -> PhotoNormalizationResult {
  return {.failure = failure, .source_dimensions = source};
}

} // namespace

auto telegram_photo_dimensions_compliant(PhotoDimensions dimensions) -> bool {
  if (dimensions.width == 0 || dimensions.height == 0) {
    return false;
  }
  const std::uint64_t width = dimensions.width;
  const std::uint64_t height = dimensions.height;
  const auto larger = std::max(width, height);
  const auto smaller = std::min(width, height);
  return width + height <= kTelegramPhotoDimensionSumLimit &&
         larger <= kTelegramPhotoDimensionRatioLimit * smaller;
}

auto inspect_photo_dimensions(std::string_view encoded) -> PhotoInspection {
  const auto dimensions = encoded_dimensions(encoded);
  if (!dimensions) {
    return {.status = PhotoDimensionStatus::InvalidDimensions};
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(dimensions->width) * dimensions->height;
  if (pixels > kTelegramPhotoDecodePixelLimit) {
    return {.status = PhotoDimensionStatus::UnsafeDimensions,
            .dimensions = *dimensions};
  }
  return {.status = telegram_photo_dimensions_compliant(*dimensions)
                        ? PhotoDimensionStatus::Compliant
                        : PhotoDimensionStatus::NeedsNormalization,
          .dimensions = *dimensions};
}

auto telegram_photo_target_geometry(PhotoDimensions dimensions)
    -> std::optional<PhotoTargetGeometry> {
  if (dimensions.width == 0 || dimensions.height == 0) {
    return std::nullopt;
  }

  std::uint64_t image_width = dimensions.width;
  std::uint64_t image_height = dimensions.height;
  std::uint64_t canvas_width = image_width;
  std::uint64_t canvas_height = image_height;

  if (canvas_width > kTelegramPhotoDimensionRatioLimit * canvas_height) {
    canvas_height = ceil_div(canvas_width, kTelegramPhotoDimensionRatioLimit);
  } else if (canvas_height > kTelegramPhotoDimensionRatioLimit * canvas_width) {
    canvas_width = ceil_div(canvas_height, kTelegramPhotoDimensionRatioLimit);
  }

  const auto canvas_sum = canvas_width + canvas_height;
  if (canvas_sum > kTelegramPhotoDimensionSumLimit) {
    const auto scaled = [canvas_sum](std::uint64_t value) {
      return std::max<std::uint64_t>(1, value * kGeometryScaleSum / canvas_sum);
    };
    image_width = scaled(image_width);
    image_height = scaled(image_height);
    canvas_width = scaled(canvas_width);
    canvas_height = scaled(canvas_height);
  }

  canvas_width = std::max(canvas_width, image_width);
  canvas_height = std::max(canvas_height, image_height);
  if (canvas_width > kTelegramPhotoDimensionRatioLimit * canvas_height) {
    canvas_height = ceil_div(canvas_width, kTelegramPhotoDimensionRatioLimit);
  } else if (canvas_height > kTelegramPhotoDimensionRatioLimit * canvas_width) {
    canvas_width = ceil_div(canvas_height, kTelegramPhotoDimensionRatioLimit);
  }

  if (image_width > dimensions.width || image_height > dimensions.height ||
      canvas_width > std::numeric_limits<std::uint32_t>::max() ||
      canvas_height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  PhotoTargetGeometry result{
      .image = {.width = static_cast<std::uint32_t>(image_width),
                .height = static_cast<std::uint32_t>(image_height)},
      .canvas = {.width = static_cast<std::uint32_t>(canvas_width),
                 .height = static_cast<std::uint32_t>(canvas_height)},
  };
  if (!telegram_photo_dimensions_compliant(result.canvas)) {
    return std::nullopt;
  }
  return result;
}

auto failure_name(PhotoNormalizationFailure failure) -> std::string_view {
  switch (failure) {
  case PhotoNormalizationFailure::None:
    return "none";
  case PhotoNormalizationFailure::InvalidDimensions:
    return "invalid_dimensions";
  case PhotoNormalizationFailure::UnsafeDimensions:
    return "unsafe_dimensions";
  case PhotoNormalizationFailure::NormalizationFailed:
    return "normalization_failed";
  }
  return "normalization_failed";
}

PhotoNormalizer::PhotoNormalizer(std::string ffmpeg_path,
                                 std::size_t output_byte_limit,
                                 ProcessRunner process_runner)
    : ffmpeg_path_(std::move(ffmpeg_path)),
      output_byte_limit_(output_byte_limit),
      process_runner_(std::move(process_runner)) {
  if (!process_runner_) {
    process_runner_ = run_process;
  }
}

auto PhotoNormalizer::normalize(DownloadedImage image) const
    -> PhotoNormalizationResult {
  const auto inspection = inspect_photo_dimensions(image.data);
  if (inspection.status == PhotoDimensionStatus::InvalidDimensions) {
    return failed_normalization(PhotoNormalizationFailure::InvalidDimensions);
  }
  if (inspection.status == PhotoDimensionStatus::UnsafeDimensions) {
    return failed_normalization(PhotoNormalizationFailure::UnsafeDimensions,
                                inspection.dimensions);
  }
  if (inspection.status == PhotoDimensionStatus::Compliant) {
    return {.image = std::move(image),
            .source_dimensions = inspection.dimensions,
            .output_dimensions = inspection.dimensions};
  }

  const auto geometry = telegram_photo_target_geometry(inspection.dimensions);
  if (!geometry || ffmpeg_path_.empty() || output_byte_limit_ == 0) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }

  TemporaryPhotoDirectory temporary;
  if (!temporary.valid()) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }
  const auto input =
      temporary.root() /
      (std::string{"input"} + std::string{input_extension(image.mime_type)});
  const auto output = temporary.root() / "output.jpg";
  {
    std::ofstream stream(input, std::ios::binary | std::ios::trunc);
    stream.write(image.data.data(),
                 static_cast<std::streamsize>(image.data.size()));
    if (!stream) {
      return failed_normalization(
          PhotoNormalizationFailure::NormalizationFailed,
          inspection.dimensions);
    }
  }

  const auto filter = fmt::format(
      "scale={}:{}:flags=lanczos,pad={}:{}:(ow-iw)/2:(oh-ih)/2:color=white",
      geometry->image.width, geometry->image.height, geometry->canvas.width,
      geometry->canvas.height);
  const std::vector<std::string> arguments = {
      ffmpeg_path_,
      "-nostdin",
      "-hide_banner",
      "-loglevel",
      "error",
      "-threads",
      "1",
      "-filter_threads",
      "1",
      "-i",
      input.string(),
      "-frames:v",
      "1",
      "-vf",
      filter,
      "-q:v",
      "4",
      "-threads",
      "1",
      "-y",
      output.string(),
  };

  ProcessResult process;
  try {
    process = process_runner_(
        arguments, std::chrono::duration_cast<std::chrono::milliseconds>(
                       kTelegramPhotoNormalizationTimeout));
  } catch (...) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }
  if (process.timed_out || process.exit_code != 0) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }

  std::error_code error;
  const auto output_size = fs::file_size(output, error);
  if (error || output_size == 0 || output_size > output_byte_limit_) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }
  std::ifstream stream(output, std::ios::binary);
  std::string normalized{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
  if (!stream && !stream.eof()) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }
  if (normalized.size() != output_size ||
      MediaProcessor::detect_mime_type_from_content(normalized) !=
          "image/jpeg") {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }
  const auto output_inspection = inspect_photo_dimensions(normalized);
  if (output_inspection.status != PhotoDimensionStatus::Compliant) {
    return failed_normalization(PhotoNormalizationFailure::NormalizationFailed,
                                inspection.dimensions);
  }

  image.filename = "qq-media-normalized.jpg";
  image.mime_type = "image/jpeg";
  image.data = std::move(normalized);
  return {.image = std::move(image),
          .normalized = true,
          .source_dimensions = inspection.dimensions,
          .output_dimensions = output_inspection.dimensions};
}

auto PhotoNormalizer::normalize_batch(std::vector<DownloadedImage> images) const
    -> std::vector<PhotoNormalizationResult> {
  std::vector<PhotoNormalizationResult> results;
  results.reserve(images.size());
  for (auto &image : images) {
    results.push_back(normalize(std::move(image)));
  }
  return results;
}

auto PhotoNormalizer::run_process(const std::vector<std::string> &arguments,
                                  std::chrono::milliseconds timeout)
    -> ProcessResult {
  if (arguments.empty() || arguments.front().empty() || timeout.count() <= 0) {
    return {};
  }

  std::array<int, 2> descriptors{};
  if (::pipe(descriptors.data()) != 0) {
    return {};
  }
  const auto child = ::fork();
  if (child == -1) {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return {};
  }
  if (child == 0) {
    (void)::setpgid(0, 0);
    ::close(descriptors[0]);
    if (::dup2(descriptors[1], STDOUT_FILENO) == -1 ||
        ::dup2(descriptors[1], STDERR_FILENO) == -1) {
      _exit(126);
    }
    ::close(descriptors[1]);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) {
      argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }

  (void)::setpgid(child, child);
  ::close(descriptors[1]);
  const auto flags = ::fcntl(descriptors[0], F_GETFL, 0);
  if (flags != -1) {
    (void)::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);
  }

  ProcessResult result;
  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 1024> buffer{};
  while (!exited) {
    while (true) {
      const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
      if (count > 0) {
        append_diagnostic_tail(result.diagnostic_tail, buffer.data(),
                               static_cast<std::size_t>(count));
        continue;
      }
      if (count == -1 && errno == EINTR) {
        continue;
      }
      break;
    }

    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child || (waited == -1 && errno == ECHILD)) {
      exited = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      (void)::kill(-child, SIGKILL);
      (void)::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {
      }
      exited = true;
      break;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    pollfd descriptor{.fd = descriptors[0], .events = POLLIN, .revents = 0};
    const auto wait_ms =
        static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 50));
    (void)::poll(&descriptor, 1, wait_ms);
  }

  while (true) {
    const auto count = ::read(descriptors[0], buffer.data(), buffer.size());
    if (count > 0) {
      append_diagnostic_tail(result.diagnostic_tail, buffer.data(),
                             static_cast<std::size_t>(count));
      continue;
    }
    if (count == -1 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(descriptors[0]);

  if (!result.timed_out && WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (!result.timed_out && WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

} // namespace bridge::qq

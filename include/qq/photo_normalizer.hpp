#pragma once

#include "qq/image_url_validator.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridge::qq {

inline constexpr std::uint64_t kTelegramPhotoDimensionSumLimit = 10000;
inline constexpr std::uint64_t kTelegramPhotoDimensionRatioLimit = 20;
inline constexpr std::uint64_t kTelegramPhotoDecodePixelLimit = 64'000'000;
inline constexpr auto kTelegramPhotoNormalizationTimeout =
    std::chrono::seconds{15};

struct PhotoDimensions {
  std::uint32_t width{0};
  std::uint32_t height{0};

  auto operator==(const PhotoDimensions &) const -> bool = default;
};

struct PhotoTargetGeometry {
  PhotoDimensions image;
  PhotoDimensions canvas;

  auto operator==(const PhotoTargetGeometry &) const -> bool = default;
};

enum class PhotoDimensionStatus : std::uint8_t {
  Compliant,
  NeedsNormalization,
  InvalidDimensions,
  UnsafeDimensions,
};

struct PhotoInspection {
  PhotoDimensionStatus status{PhotoDimensionStatus::InvalidDimensions};
  PhotoDimensions dimensions;
};

[[nodiscard]] auto inspect_photo_dimensions(std::string_view encoded)
    -> PhotoInspection;
[[nodiscard]] auto telegram_photo_dimensions_compliant(
    PhotoDimensions dimensions) -> bool;
[[nodiscard]] auto telegram_photo_target_geometry(PhotoDimensions dimensions)
    -> std::optional<PhotoTargetGeometry>;

enum class PhotoNormalizationFailure : std::uint8_t {
  None,
  InvalidDimensions,
  UnsafeDimensions,
  NormalizationFailed,
};

struct PhotoNormalizationResult {
  std::optional<DownloadedImage> image;
  PhotoNormalizationFailure failure{PhotoNormalizationFailure::None};
  bool normalized{false};
  PhotoDimensions source_dimensions;
  PhotoDimensions output_dimensions;

  [[nodiscard]] auto succeeded() const -> bool { return image.has_value(); }
};

[[nodiscard]] auto failure_name(PhotoNormalizationFailure failure)
    -> std::string_view;

class PhotoNormalizer {
public:
  struct ProcessResult {
    int exit_code{-1};
    bool timed_out{false};
    std::string diagnostic_tail;
  };

  using ProcessRunner = std::function<ProcessResult(
      const std::vector<std::string> &, std::chrono::milliseconds)>;

  PhotoNormalizer(std::string ffmpeg_path, std::size_t output_byte_limit,
                  ProcessRunner process_runner = {});

  auto normalize(DownloadedImage image) const -> PhotoNormalizationResult;
  auto normalize_batch(std::vector<DownloadedImage> images) const
      -> std::vector<PhotoNormalizationResult>;

  static auto run_process(const std::vector<std::string> &arguments,
                          std::chrono::milliseconds timeout) -> ProcessResult;

private:
  std::string ffmpeg_path_;
  std::size_t output_byte_limit_;
  ProcessRunner process_runner_;
};

} // namespace bridge::qq

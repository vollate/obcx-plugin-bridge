#include "qq/photo_normalizer.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using bridge::qq::DownloadedImage;
using bridge::qq::PhotoDimensions;
using bridge::qq::PhotoDimensionStatus;
using bridge::qq::PhotoNormalizationFailure;
using bridge::qq::PhotoNormalizer;

void put_be16(std::string &data, std::size_t offset, std::uint16_t value) {
  data[offset] = static_cast<char>((value >> 8U) & 0xffU);
  data[offset + 1] = static_cast<char>(value & 0xffU);
}

void put_be32(std::string &data, std::size_t offset, std::uint32_t value) {
  data[offset] = static_cast<char>((value >> 24U) & 0xffU);
  data[offset + 1] = static_cast<char>((value >> 16U) & 0xffU);
  data[offset + 2] = static_cast<char>((value >> 8U) & 0xffU);
  data[offset + 3] = static_cast<char>(value & 0xffU);
}

void put_le16(std::string &data, std::size_t offset, std::uint16_t value) {
  data[offset] = static_cast<char>(value & 0xffU);
  data[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
}

void put_le32(std::string &data, std::size_t offset, std::uint32_t value) {
  data[offset] = static_cast<char>(value & 0xffU);
  data[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
  data[offset + 2] = static_cast<char>((value >> 16U) & 0xffU);
  data[offset + 3] = static_cast<char>((value >> 24U) & 0xffU);
}

auto jpeg(std::uint16_t width, std::uint16_t height) -> std::string {
  std::string data(21, '\0');
  data[0] = static_cast<char>(0xff);
  data[1] = static_cast<char>(0xd8);
  data[2] = static_cast<char>(0xff);
  data[3] = static_cast<char>(0xc0);
  put_be16(data, 4, 17);
  data[6] = 8;
  put_be16(data, 7, height);
  put_be16(data, 9, width);
  data[11] = 3;
  return data;
}

auto png(std::uint32_t width, std::uint32_t height) -> std::string {
  std::string data(24, '\0');
  const std::string signature{"\x89PNG\r\n\x1a\n", 8};
  data.replace(0, signature.size(), signature);
  put_be32(data, 8, 13);
  data.replace(12, 4, "IHDR");
  put_be32(data, 16, width);
  put_be32(data, 20, height);
  return data;
}

auto gif(std::uint16_t width, std::uint16_t height) -> std::string {
  std::string data(10, '\0');
  data.replace(0, 6, "GIF89a");
  put_le16(data, 6, width);
  put_le16(data, 8, height);
  return data;
}

auto webp(std::uint32_t width, std::uint32_t height) -> std::string {
  std::string data(30, '\0');
  data.replace(0, 4, "RIFF");
  data.replace(8, 4, "WEBP");
  data.replace(12, 4, "VP8X");
  put_le32(data, 16, 10);
  const auto encoded_width = width - 1;
  const auto encoded_height = height - 1;
  data[24] = static_cast<char>(encoded_width & 0xffU);
  data[25] = static_cast<char>((encoded_width >> 8U) & 0xffU);
  data[26] = static_cast<char>((encoded_width >> 16U) & 0xffU);
  data[27] = static_cast<char>(encoded_height & 0xffU);
  data[28] = static_cast<char>((encoded_height >> 8U) & 0xffU);
  data[29] = static_cast<char>((encoded_height >> 16U) & 0xffU);
  return data;
}

auto bmp(std::uint32_t width, std::uint32_t height) -> std::string {
  std::string data(54, '\0');
  data.replace(0, 2, "BM");
  put_le32(data, 14, 40);
  put_le32(data, 18, width);
  put_le32(data, 22, height);
  return data;
}

auto image(std::string data, std::string mime = "image/jpeg")
    -> DownloadedImage {
  return {.type = "photo",
          .original_url = "sensitive-signed-url",
          .filename = "source.jpg",
          .mime_type = std::move(mime),
          .data = std::move(data)};
}

auto temporary_photo_directories() -> std::size_t {
  std::error_code error;
  std::size_t count = 0;
  for (const auto &entry :
       fs::directory_iterator(fs::temp_directory_path(), error)) {
    if (!error && entry.is_directory() &&
        entry.path().filename().string().starts_with("obcx-qq-photo-")) {
      ++count;
    }
  }
  return count;
}

TEST(PhotoNormalizerTest, InspectsSupportedEncodedDimensionHeaders) {
  const std::vector<std::pair<std::string, PhotoDimensions>> cases = {
      {jpeg(640, 480), {640, 480}},    {png(800, 600), {800, 600}},
      {gif(320, 240), {320, 240}},     {webp(1024, 768), {1024, 768}},
      {bmp(1920, 1080), {1920, 1080}},
  };
  for (const auto &[encoded, expected] : cases) {
    const auto inspected = bridge::qq::inspect_photo_dimensions(encoded);
    EXPECT_EQ(inspected.status, PhotoDimensionStatus::Compliant);
    EXPECT_EQ(inspected.dimensions, expected);
  }
}

TEST(PhotoNormalizerTest, EnforcesBoundariesAndRejectsMalformedDimensions) {
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(5000, 5000)).status,
            PhotoDimensionStatus::Compliant);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(5001, 5000)).status,
            PhotoDimensionStatus::NeedsNormalization);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(2000, 100)).status,
            PhotoDimensionStatus::Compliant);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(2001, 100)).status,
            PhotoDimensionStatus::NeedsNormalization);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(8001, 8000)).status,
            PhotoDimensionStatus::UnsafeDimensions);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions("not-an-image").status,
            PhotoDimensionStatus::InvalidDimensions);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(png(0, 10)).status,
            PhotoDimensionStatus::InvalidDimensions);
  auto truncated = jpeg(640, 480);
  truncated.resize(8);
  EXPECT_EQ(bridge::qq::inspect_photo_dimensions(truncated).status,
            PhotoDimensionStatus::InvalidDimensions);
}

TEST(PhotoNormalizerTest, ComputesRegressionAndExtremeRatioGeometry) {
  const auto regression = bridge::qq::telegram_photo_target_geometry(
      PhotoDimensions{.width = 2048, .height = 13301});
  ASSERT_TRUE(regression.has_value());
  EXPECT_EQ(regression->image, (PhotoDimensions{1332, 8657}));
  EXPECT_EQ(regression->canvas, regression->image);
  EXPECT_TRUE(
      bridge::qq::telegram_photo_dimensions_compliant(regression->canvas));

  const auto extreme = bridge::qq::telegram_photo_target_geometry(
      PhotoDimensions{.width = 1, .height = 10000});
  ASSERT_TRUE(extreme.has_value());
  EXPECT_EQ(extreme->image, (PhotoDimensions{1, 9514}));
  EXPECT_EQ(extreme->canvas, (PhotoDimensions{476, 9514}));
  EXPECT_LE(extreme->image.width, 1U);
  EXPECT_LE(extreme->image.height, 10000U);
  EXPECT_TRUE(bridge::qq::telegram_photo_dimensions_compliant(extreme->canvas));
}

TEST(PhotoNormalizerTest, LeavesCompliantBytesUntouchedWithoutRunningFfmpeg) {
  const auto original = jpeg(1024, 372);
  int calls = 0;
  PhotoNormalizer normalizer(
      "unused", 1024,
      [&](const auto &, auto) -> PhotoNormalizer::ProcessResult {
        ++calls;
        return {};
      });
  const auto result = normalizer.normalize(image(original));

  ASSERT_TRUE(result.succeeded());
  EXPECT_FALSE(result.normalized);
  EXPECT_EQ(result.image->data, original);
  EXPECT_EQ(result.image->original_url, "sensitive-signed-url");
  EXPECT_EQ(calls, 0);
}

TEST(PhotoNormalizerTest, NormalizesOverlongPhotoAndValidatesOutput) {
  std::vector<std::string> seen_arguments;
  fs::path temporary_root;
  PhotoNormalizer normalizer(
      "fake-ffmpeg", 1024,
      [&](const std::vector<std::string> &arguments,
          auto timeout) -> PhotoNormalizer::ProcessResult {
        seen_arguments = arguments;
        EXPECT_EQ(timeout, std::chrono::seconds(15));
        EXPECT_NE(std::ranges::find(arguments, std::string{"-threads"}),
                  arguments.end());
        EXPECT_NE(std::ranges::find(arguments, std::string{"1"}),
                  arguments.end());
        const fs::path output = arguments.back();
        temporary_root = output.parent_path();
        std::ofstream stream(output, std::ios::binary);
        const auto transformed = jpeg(1332, 8657);
        stream.write(transformed.data(),
                     static_cast<std::streamsize>(transformed.size()));
        return {.exit_code = 0};
      });

  const auto result = normalizer.normalize(image(jpeg(2048, 13301)));
  ASSERT_TRUE(result.succeeded());
  EXPECT_TRUE(result.normalized);
  EXPECT_EQ(result.source_dimensions, (PhotoDimensions{2048, 13301}));
  EXPECT_EQ(result.output_dimensions, (PhotoDimensions{1332, 8657}));
  EXPECT_EQ(result.image->mime_type, "image/jpeg");
  EXPECT_EQ(result.image->original_url, "sensitive-signed-url");
  EXPECT_FALSE(seen_arguments.empty());
  EXPECT_FALSE(fs::exists(temporary_root));
}

TEST(PhotoNormalizerTest, RejectsInvalidUnsafeAndFailedTransformations) {
  int calls = 0;
  PhotoNormalizer normalizer("fake-ffmpeg", 128,
                             [&](const std::vector<std::string> &arguments,
                                 auto) -> PhotoNormalizer::ProcessResult {
                               ++calls;
                               std::ofstream(arguments.back(), std::ios::binary)
                                   << "bad-output";
                               return {.exit_code = 0};
                             });

  auto invalid = normalizer.normalize(image("bad"));
  EXPECT_EQ(invalid.failure, PhotoNormalizationFailure::InvalidDimensions);
  auto unsafe = normalizer.normalize(image(png(8001, 8000), "image/png"));
  EXPECT_EQ(unsafe.failure, PhotoNormalizationFailure::UnsafeDimensions);
  auto failed = normalizer.normalize(image(jpeg(2048, 13301)));
  EXPECT_EQ(failed.failure, PhotoNormalizationFailure::NormalizationFailed);
  EXPECT_EQ(calls, 1);
}

TEST(PhotoNormalizerTest,
     TimeoutExceptionAndOversizedOutputCleanTemporaryData) {
  const auto before = temporary_photo_directories();
  for (int mode = 0; mode < 3; ++mode) {
    PhotoNormalizer normalizer(
        "fake-ffmpeg", 32,
        [mode](const std::vector<std::string> &arguments,
               auto) -> PhotoNormalizer::ProcessResult {
          if (mode == 0) {
            return {.exit_code = -1, .timed_out = true};
          }
          if (mode == 1) {
            throw std::runtime_error("cancelled runner");
          }
          std::ofstream stream(arguments.back(), std::ios::binary);
          auto output = jpeg(1332, 8657);
          output.append(64, 'x');
          stream.write(output.data(),
                       static_cast<std::streamsize>(output.size()));
          return {.exit_code = 0};
        });
    const auto result = normalizer.normalize(image(jpeg(2048, 13301)));
    EXPECT_EQ(result.failure, PhotoNormalizationFailure::NormalizationFailed);
  }
  EXPECT_EQ(temporary_photo_directories(), before);
}

TEST(PhotoNormalizerTest, BatchNormalizationIsSequentialAndOrdered) {
  int active = 0;
  int max_active = 0;
  int calls = 0;
  PhotoNormalizer normalizer(
      "fake-ffmpeg", 1024,
      [&](const std::vector<std::string> &arguments,
          auto) -> PhotoNormalizer::ProcessResult {
        ++active;
        max_active = std::max(max_active, active);
        ++calls;
        const auto output = jpeg(1332, 8657);
        std::ofstream stream(arguments.back(), std::ios::binary);
        stream.write(output.data(),
                     static_cast<std::streamsize>(output.size()));
        --active;
        return {.exit_code = 0};
      });

  std::vector<DownloadedImage> images;
  images.push_back(image(jpeg(2048, 13301)));
  images.push_back(image(jpeg(1024, 372)));
  images.push_back(image(jpeg(2048, 13301)));
  const auto results = normalizer.normalize_batch(std::move(images));

  ASSERT_EQ(results.size(), 3U);
  EXPECT_TRUE(results[0].normalized);
  EXPECT_FALSE(results[1].normalized);
  EXPECT_TRUE(results[2].normalized);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(max_active, 1);
}

TEST(PhotoNormalizerTest, DirectProcessRunnerPreservesArgumentsAndTimesOut) {
  const auto echoed = PhotoNormalizer::run_process(
      {"/bin/sh", "-c", "printf '%s' \"$1\"", "sh", "value with spaces"},
      std::chrono::seconds(1));
  EXPECT_EQ(echoed.exit_code, 0);
  EXPECT_FALSE(echoed.timed_out);
  EXPECT_EQ(echoed.diagnostic_tail, "value with spaces");

  const auto started = std::chrono::steady_clock::now();
  const auto timed_out = PhotoNormalizer::run_process(
      {"/bin/sh", "-c", "sleep 2"}, std::chrono::milliseconds(50));
  EXPECT_TRUE(timed_out.timed_out);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));
}

} // namespace

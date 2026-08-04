#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bridge {
struct BridgeConfig;
}

namespace bridge::qq {

/**
 * @brief 单个媒体URL在批量校验中的状态
 */
enum class ImageUrlStatus : std::uint8_t {
  Reachable, // URL 校验通过，可以直接交给 Telegram 拉取
  Replaced,  // URL 校验失败，已替换为占位图
};

struct ImageUrlValidation {
  std::string original_url; // 原始 QQ URL
  std::string
      effective_url;     // 校验后实际用于发送的 URL（成功时同 original_url）
  ImageUrlStatus status; // 单条校验结果
  std::string failure_reason; // 仅在失败时填充，便于日志/提示
};

struct DownloadedImage {
  std::string type;
  std::string original_url;
  std::string filename;
  std::string mime_type;
  std::string data;
};

enum class MediaDownloadFailure : std::uint8_t {
  None,
  InvalidUrl,
  Transport,
  HttpStatus,
  OverLimit,
  EmptyBody,
  InvalidImage,
};

struct MediaDownloadResult {
  std::optional<DownloadedImage> image;
  MediaDownloadFailure failure{MediaDownloadFailure::None};
  std::string diagnostic;

  [[nodiscard]] auto succeeded() const -> bool { return image.has_value(); }
};

/**
 * @brief QQ 图片 URL 预校验器
 *
 * 在调用 Telegram 的 sendMediaGroup 之前，先用 HTTP 请求探测 QQ 图片 URL
 * 是否可达。Telegram 的服务器在拉取 media 时是「整批原子」语义：只要其中
 * 一条 URL 拉取失败，整批 sendMediaGroup 都会返回错误（"failed to get
 * HTTP URL content"），导致同一条 QQ 群消息中可能成功的其它图片也无法转发。
 *
 * 本组件按以下策略缓解：
 *   1. 对每个 URL 用 GET (Range: bytes=0-0) 探测；
 *   2. 失败时按 actor 代际配置做指数退避重试；
 *   3. 仍失败的 URL 一律替换为该代配置的占位图，
 *      保证该批次仍能整体提交给 Telegram；
 *   4. 多个 URL 的探测在同一个执行器上并发进行，整体延迟接近最慢的那条。
 */
class ImageUrlValidator {
public:
  /**
   * @brief 校验一个 (type, url) 列表。
   *
   * @param media 输入的 (type, url) 序列
   * @return 与输入等长的校验结果。Caller 根据 status 决定如何重组 media_list。
   */
  static auto validate(
      const bridge::BridgeConfig &config,
      const std::vector<std::pair<std::string, std::string>> &media)
      -> boost::asio::awaitable<std::vector<ImageUrlValidation>>;

  /**
   * @brief 便捷封装：直接对 (type, url) 列表做校验+占位替换，返回最终
   *        可发送的 (type, url) 列表。失败的原始 URL 通过 replaced_urls
   *        输出，方便上层把失败数量提示给用户。
   */
  static auto sanitize(
      const bridge::BridgeConfig &config,
      const std::vector<std::pair<std::string, std::string>> &media,
      std::vector<std::string> &replaced_urls)
      -> boost::asio::awaitable<
          std::vector<std::pair<std::string, std::string>>>;

  /** Download effective media URLs into order-preserving item outcomes. */
  static auto download(
      const bridge::BridgeConfig &config,
      const std::vector<std::pair<std::string, std::string>> &media)
      -> boost::asio::awaitable<std::vector<MediaDownloadResult>>;
};

} // namespace bridge::qq

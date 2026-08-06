#include "qq/image_url_validator.hpp"

#include "config.hpp"
#include "media_processor.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <charconv>
#include <chrono>
#include <common/logger.hpp>
#include <map>
#include <memory>
#include <network/http_client.hpp>
#include <optional>
#include <stdexcept>

namespace bridge::qq {

namespace {

namespace asio = boost::asio;

constexpr unsigned int kHttpsPort = 443;
constexpr unsigned int kHttpPort = 80;
constexpr unsigned int kHttpStatusOk = 200;
constexpr unsigned int kHttpStatusPartialContent = 206;

struct ParsedUrl {
  bool valid{false};
  bool use_ssl{false};
  std::string host;
  std::uint16_t port{0};
  std::string path;
};

class DownloadError : public std::runtime_error {
public:
  DownloadError(MediaDownloadFailure failure, std::string message)
      : std::runtime_error(std::move(message)), failure_(failure) {}

  [[nodiscard]] auto failure() const -> MediaDownloadFailure {
    return failure_;
  }

private:
  MediaDownloadFailure failure_;
};

auto parse_url(const std::string &url) -> ParsedUrl {
  ParsedUrl parsed;
  if (url.starts_with("https://")) {
    parsed.use_ssl = true;
  } else if (url.starts_with("http://")) {
    parsed.use_ssl = false;
  } else {
    return parsed;
  }

  const std::size_t authority_start = url.find("://") + 3;
  const std::size_t authority_end = url.find_first_of("/?#", authority_start);
  std::string authority =
      url.substr(authority_start, authority_end - authority_start);
  parsed.path =
      authority_end == std::string::npos
          ? "/"
          : (url[authority_end] == '/' ? url.substr(authority_end)
                                       : "/" + url.substr(authority_end));

  parsed.port = parsed.use_ssl ? kHttpsPort : kHttpPort;
  const auto port_separator = authority.rfind(':');
  if (port_separator != std::string::npos &&
      authority.find(':') == port_separator) {
    const auto port_text =
        std::string_view{authority}.substr(port_separator + 1);
    unsigned int port = 0;
    const auto [end, error] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() ||
        port == 0 || port > 65535) {
      return parsed;
    }
    authority.resize(port_separator);
    parsed.port = static_cast<std::uint16_t>(port);
  }

  parsed.host = std::move(authority);
  parsed.valid = !parsed.host.empty();
  return parsed;
}

// 用 Range: bytes=0-0 探测 URL 是否可达：部分 QQ 镜像服务器对 HEAD 返回 405，
// 但 Range: bytes=0-0 在所有常见图床上都能稳定工作，且只下载 1 字节。
// 每次都创建临时直连 HttpClient（无代理），与 detect_gif_format 一致——
// QQ 图片下载不应走 Telegram 代理。
auto probe_once(const ParsedUrl &url, std::chrono::milliseconds timeout)
    -> asio::awaitable<bool> {
  asio::io_context temp_ioc;

  obcx::common::ConnectionConfig cfg;
  cfg.host = url.host;
  cfg.port = url.port;
  cfg.use_ssl = url.use_ssl;
  cfg.access_token.clear();
  cfg.proxy_host.clear();
  cfg.proxy_port = 0;
  cfg.proxy_type.clear();
  cfg.proxy_username.clear();
  cfg.proxy_password.clear();

  auto client = std::make_unique<obcx::network::HttpClient>(temp_ioc, cfg);
  client->set_timeout(timeout);

  std::map<std::string, std::string> headers;
  headers["Range"] = "bytes=0-0";

  obcx::network::HttpResponse resp = co_await client->get(url.path, headers);
  // 200 OK / 206 Partial Content 都视为可达。
  // 部分服务器忽略 Range 直接返回 200 也接受。
  co_return resp.status_code == kHttpStatusOk ||
      resp.status_code == kHttpStatusPartialContent;
}

auto extension_for_mime(std::string_view mime) -> std::string_view {
  if (mime == "image/jpeg") {
    return ".jpg";
  }
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
  return ".bin";
}

auto download_once(const ParsedUrl &url, std::chrono::milliseconds timeout,
                   std::size_t body_limit)
    -> asio::awaitable<std::pair<std::string, std::string>> {
  asio::io_context temp_ioc;

  obcx::common::ConnectionConfig cfg;
  cfg.host = url.host;
  cfg.port = url.port;
  cfg.use_ssl = url.use_ssl;
  cfg.access_token.clear();
  cfg.proxy_host.clear();
  cfg.proxy_port = 0;
  cfg.proxy_type.clear();
  cfg.proxy_username.clear();
  cfg.proxy_password.clear();

  auto client = std::make_unique<obcx::network::HttpClient>(temp_ioc, cfg);
  client->set_timeout(timeout);
  client->set_response_body_limit(body_limit);

  obcx::network::HttpResponse response;
  try {
    response =
        co_await client->get(url.path, {{"Accept-Encoding", "identity"}});
  } catch (const obcx::network::HttpClientError &error) {
    if (std::string_view{error.what()}.find("body limit exceeded") !=
        std::string_view::npos) {
      throw DownloadError(MediaDownloadFailure::OverLimit,
                          "response exceeds configured media limit");
    }
    throw DownloadError(MediaDownloadFailure::Transport,
                        "media transport failed");
  }
  if (!response.is_success()) {
    throw DownloadError(MediaDownloadFailure::HttpStatus,
                        "media server returned non-success status");
  }
  if (response.body.empty()) {
    throw DownloadError(MediaDownloadFailure::EmptyBody,
                        "media response body is empty");
  }
  auto mime =
      bridge::MediaProcessor::detect_mime_type_from_content(response.body);
  if (mime.empty()) {
    throw DownloadError(MediaDownloadFailure::InvalidImage,
                        "media response is not a recognized image");
  }
  co_return std::pair{std::move(response.body), std::move(mime)};
}

// 单 URL 的指数退避探测：base, base*2, base*4 … 最多 max_attempts 次。
// 任一次成功即返回 true；全部失败返回 false 并把最后一次错误写入 fail_reason。
auto probe_with_backoff(const BridgeConfig &config, std::string url,
                        std::string &fail_reason) -> asio::awaitable<bool> {
  const int max_attempts = std::max(1, config.image_url_probe_max_attempts);
  const auto base_delay = std::chrono::milliseconds(
      std::max(1, config.image_url_probe_base_delay_ms));
  const auto per_attempt_timeout =
      std::chrono::milliseconds(std::max(1, config.image_url_probe_timeout_ms));

  ParsedUrl parsed = parse_url(url);
  if (!parsed.valid) {
    fail_reason = "invalid url scheme/host";
    co_return false;
  }

  auto executor = co_await asio::this_coro::executor;

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    try {
      bool ok = co_await probe_once(parsed, per_attempt_timeout);
      if (ok) {
        co_return true;
      }
      fail_reason = "probe returned non-success status";
    } catch (const std::exception &e) {
      fail_reason = e.what();
    }

    if (attempt + 1 < max_attempts) {
      auto delay = base_delay * (1U << static_cast<unsigned>(attempt));
      asio::steady_timer timer(executor, delay);
      boost::system::error_code ec;
      co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
      // timer 被取消（ec=operation_aborted）时也继续下一次尝试。
    }
  }
  co_return false;
}

} // namespace

auto ImageUrlValidator::validate(
    const BridgeConfig &config,
    const std::vector<std::pair<std::string, std::string>> &media)
    -> asio::awaitable<std::vector<ImageUrlValidation>> {
  std::vector<ImageUrlValidation> results(media.size());
  for (std::size_t i = 0; i < media.size(); ++i) {
    results[i].original_url = media[i].second;
    results[i].effective_url = media[i].second;
    results[i].status = ImageUrlStatus::Reachable;
  }

  if (media.empty()) {
    co_return results;
  }

  auto executor = co_await asio::this_coro::executor;

  // 用一个共享 atomic 计数 + steady_timer 当 barrier，等所有探测协程完成。
  auto remaining = std::make_shared<std::atomic<std::size_t>>(media.size());
  auto barrier = std::make_shared<asio::steady_timer>(
      executor, std::chrono::steady_clock::time_point::max());

  auto reasons = std::make_shared<std::vector<std::string>>(media.size());
  auto reachable = std::make_shared<std::vector<char>>(media.size(), 0);

  for (std::size_t i = 0; i < media.size(); ++i) {
    asio::co_spawn(
        executor,
        [i, &config, url = media[i].second, remaining, barrier, reasons,
         reachable]() -> asio::awaitable<void> {
          std::string reason;
          bool ok = false;
          try {
            ok = co_await probe_with_backoff(config, url, reason);
          } catch (const std::exception &e) {
            reason = e.what();
          }
          (*reachable)[i] = ok ? 1 : 0;
          (*reasons)[i] = std::move(reason);

          if (remaining->fetch_sub(1) == 1) {
            barrier->cancel();
          }
          co_return;
        },
        asio::detached);
  }

  // 等所有探测完成。无效 URL 可能在 co_spawn 启动时同步完成并先于
  // async_wait 调用 cancel()；此时不能再挂一个永不到期的等待。
  if (remaining->load(std::memory_order_acquire) != 0) {
    boost::system::error_code ec;
    co_await barrier->async_wait(asio::redirect_error(asio::use_awaitable, ec));
  }

  const std::string &configured = config.image_placeholder_url;
  // 即使用户在配置里把占位图清空，也要保证有一个内置兜底，否则替换语义会变成
  // 「丢弃」，那就违反了「失败 == 替换」的约定。
  static constexpr std::string_view kBuiltinPlaceholder =
      "https://placehold.co/512x512/e9ecef/495057/png?text=NOT+FOUND";
  const std::string placeholder =
      configured.empty() ? std::string(kBuiltinPlaceholder) : configured;
  for (std::size_t i = 0; i < media.size(); ++i) {
    if ((*reachable)[i] != 0) {
      results[i].status = ImageUrlStatus::Reachable;
      continue;
    }
    results[i].failure_reason = (*reasons)[i];
    results[i].effective_url = placeholder;
    results[i].status = ImageUrlStatus::Replaced;
    OBCX_WARN("[图片URL校验] 探测失败，使用占位图替换: index={}, "
              "category=probe_failed",
              i + 1);
  }
  co_return results;
}

auto ImageUrlValidator::sanitize(
    const BridgeConfig &config,
    const std::vector<std::pair<std::string, std::string>> &media,
    std::vector<std::string> &replaced_urls)
    -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
  replaced_urls.clear();

  auto results = co_await validate(config, media);

  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(media.size());
  for (std::size_t i = 0; i < media.size(); ++i) {
    out.emplace_back(media[i].first, results[i].effective_url);
    if (results[i].status == ImageUrlStatus::Replaced) {
      replaced_urls.push_back(results[i].original_url);
    }
  }
  co_return out;
}

auto ImageUrlValidator::download(
    const BridgeConfig &config,
    const std::vector<std::pair<std::string, std::string>> &media)
    -> asio::awaitable<std::vector<MediaDownloadResult>> {
  if (media.empty()) {
    co_return std::vector<MediaDownloadResult>{};
  }

  std::vector<MediaDownloadResult> results(media.size());
  std::atomic<std::size_t> next_index{0};
  const auto timeout = std::chrono::milliseconds(
      std::max(30000, config.image_url_probe_timeout_ms));
  const auto body_limit = config.qq_media_download_max_bytes;

  auto worker = [&]() -> asio::awaitable<void> {
    while (true) {
      const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
      if (index >= media.size()) {
        co_return;
      }

      const auto &item = media[index];
      try {
        const auto parsed = parse_url(item.second);
        if (!parsed.valid) {
          throw DownloadError(MediaDownloadFailure::InvalidUrl,
                              "invalid media URL");
        }
        auto [data, mime] = co_await download_once(parsed, timeout, body_limit);
        results[index].image = DownloadedImage{
            .type = item.first.empty() ? "photo" : item.first,
            .original_url = item.second,
            .filename = "qq-media-" + std::to_string(index) +
                        std::string{extension_for_mime(mime)},
            .mime_type = std::move(mime),
            .data = std::move(data),
        };
      } catch (const DownloadError &error) {
        results[index].failure = error.failure();
        results[index].diagnostic = error.what();
      } catch (const boost::system::system_error &error) {
        if (error.code() == asio::error::operation_aborted) {
          throw;
        }
        results[index].failure = MediaDownloadFailure::Transport;
        results[index].diagnostic = "media transport failed";
      } catch (const std::exception &) {
        results[index].failure = MediaDownloadFailure::Transport;
        results[index].diagnostic = "media transport failed";
      }
    }
  };

  using namespace asio::experimental::awaitable_operators;
  co_await (worker() && worker() && worker());
  co_return results;
}

} // namespace bridge::qq

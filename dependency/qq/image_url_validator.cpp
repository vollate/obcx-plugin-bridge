#include "qq/image_url_validator.hpp"

#include "config.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <common/logger.hpp>
#include <map>
#include <memory>
#include <network/http_client.hpp>

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
  std::string path; // 始终以 '/' 开头
};

// 拆 http(s):// 链接为 host / path。不做 query/fragment 拆分——
// host 之后所有内容（含 query）一律作为 path 传给 HttpClient。
auto parse_url(const std::string &url) -> ParsedUrl {
  ParsedUrl p;
  if (url.starts_with("https://")) {
    p.use_ssl = true;
  } else if (url.starts_with("http://")) {
    p.use_ssl = false;
  } else {
    return p;
  }

  const std::size_t scheme_end = url.find("://");
  const std::size_t host_start = scheme_end + 3;
  const std::size_t path_start = url.find('/', host_start);

  if (path_start == std::string::npos) {
    p.host = url.substr(host_start);
    p.path = "/";
  } else {
    p.host = url.substr(host_start, path_start - host_start);
    p.path = url.substr(path_start);
  }
  if (p.host.empty()) {
    return p;
  }
  p.valid = true;
  return p;
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
  cfg.port = url.use_ssl ? kHttpsPort : kHttpPort;
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

// 单 URL 的指数退避探测：base, base*2, base*4 … 最多 max_attempts 次。
// 任一次成功即返回 true；全部失败返回 false 并把最后一次错误写入 fail_reason。
auto probe_with_backoff(std::string url, std::string &fail_reason)
    -> asio::awaitable<bool> {
  const int max_attempts = std::max(1, config::IMAGE_URL_PROBE_MAX_ATTEMPTS);
  const auto base_delay = std::chrono::milliseconds(
      std::max(1, config::IMAGE_URL_PROBE_BASE_DELAY_MS));
  const auto per_attempt_timeout = std::chrono::milliseconds(
      std::max(1, config::IMAGE_URL_PROBE_TIMEOUT_MS));

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
        [i, url = media[i].second, remaining, barrier, reasons,
         reachable]() -> asio::awaitable<void> {
          std::string reason;
          bool ok = false;
          try {
            ok = co_await probe_with_backoff(url, reason);
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

  // 等所有探测完成。barrier 设置在「无穷远」未来，仅靠 cancel() 唤醒。
  boost::system::error_code ec;
  co_await barrier->async_wait(asio::redirect_error(asio::use_awaitable, ec));

  const std::string &configured = config::IMAGE_PLACEHOLDER_URL;
  // 即使用户在配置里把占位图清空，也要保证有一个内置兜底，否则替换语义会变成
  // 「丢弃」，那就违反了「失败 == 替换」的约定。
  static constexpr std::string_view kBuiltinPlaceholder =
      "https://placehold.co/512x512/cccccc/666666/png?text=Image+Unavailable";
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
    PLUGIN_WARN("qq_to_tg",
                "[图片URL校验] 探测失败，使用占位图替换: {} (reason: {})",
                results[i].original_url, results[i].failure_reason);
  }
  co_return results;
}

auto ImageUrlValidator::sanitize(
    const std::vector<std::pair<std::string, std::string>> &media,
    std::vector<std::string> &replaced_urls)
    -> asio::awaitable<std::vector<std::pair<std::string, std::string>>> {
  replaced_urls.clear();

  auto results = co_await validate(media);

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

} // namespace bridge::qq

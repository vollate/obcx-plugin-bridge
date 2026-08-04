#include "config.hpp"
#include "qq/image_url_validator.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace bridge::qq::test {

namespace {

auto png_body(std::size_t size) -> std::string {
  static constexpr unsigned char header[] = {0x89, 0x50, 0x4e, 0x47,
                                             0x0d, 0x0a, 0x1a, 0x0a};
  std::string body(std::max(size, sizeof(header)), 'x');
  std::copy(std::begin(header), std::end(header), body.begin());
  return body;
}

template <typename T>
auto run_awaitable(asio::io_context &ioc, asio::awaitable<T> operation) -> T {
  auto future = asio::co_spawn(ioc, std::move(operation), asio::use_future);
  ioc.run();
  ioc.restart();
  return future.get();
}

class MediaServer {
public:
  MediaServer()
      : endpoint_(asio::ip::make_address("127.0.0.1"), 0), acceptor_(ioc_),
        work_(asio::make_work_guard(ioc_)) {
    acceptor_.open(endpoint_.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint_);
    acceptor_.listen();
    endpoint_ = acceptor_.local_endpoint();
  }

  ~MediaServer() { stop(); }

  void start() {
    accept();
    thread_ = std::thread([this] { ioc_.run(); });
  }

  void stop() {
    if (!thread_.joinable()) {
      return;
    }
    asio::post(ioc_, [this] {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      work_.reset();
    });
    thread_.join();
  }

  void set(std::string path, std::string body, bool chunked = false,
           std::chrono::milliseconds delay = 0ms) {
    std::scoped_lock lock(mutex_);
    responses_.insert_or_assign(std::move(path),
                                Response{std::move(body), chunked, delay});
  }

  [[nodiscard]] auto url(std::string_view path) const -> std::string {
    return "http://127.0.0.1:" + std::to_string(endpoint_.port()) +
           std::string(path);
  }

  [[nodiscard]] auto max_active() const -> int { return max_active_.load(); }
  [[nodiscard]] auto active() const -> int { return active_.load(); }
  [[nodiscard]] auto identity_requests() const -> int {
    return identity_requests_.load();
  }

private:
  struct Response {
    std::string body;
    bool chunked{false};
    std::chrono::milliseconds delay{0};
  };

  void accept() {
    acceptor_.async_accept([this](beast::error_code error, tcp::socket socket) {
      if (!error) {
        read(std::move(socket));
      }
      if (acceptor_.is_open()) {
        accept();
      }
    });
  }

  void read(tcp::socket socket) {
    auto stream = std::make_shared<tcp::socket>(std::move(socket));
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto request = std::make_shared<http::request<http::string_body>>();
    http::async_read(
        *stream, *buffer, *request,
        [this, stream, buffer, request](beast::error_code error, std::size_t) {
          if (error) {
            return;
          }
          if ((*request)[http::field::accept_encoding] == "identity") {
            identity_requests_.fetch_add(1);
          }

          Response selected;
          {
            std::scoped_lock lock(mutex_);
            const auto found = responses_.find(std::string(request->target()));
            if (found == responses_.end()) {
              selected.body = "not found";
            } else {
              selected = found->second;
            }
          }

          const int now = active_.fetch_add(1) + 1;
          int observed = max_active_.load();
          while (now > observed &&
                 !max_active_.compare_exchange_weak(observed, now)) {
          }

          auto timer =
              std::make_shared<asio::steady_timer>(ioc_, selected.delay);
          timer->async_wait([this, stream, timer,
                             selected =
                                 std::move(selected)](beast::error_code) {
            auto response = std::make_shared<http::response<http::string_body>>(
                http::status::ok, 11);
            response->set(http::field::content_type, "image/png");
            response->body() = selected.body;
            if (selected.chunked) {
              response->chunked(true);
            } else {
              response->prepare_payload();
            }
            http::async_write(
                *stream, *response,
                [this, stream, response](beast::error_code, std::size_t) {
                  active_.fetch_sub(1);
                  boost::system::error_code ignored;
                  stream->shutdown(tcp::socket::shutdown_both, ignored);
                });
          });
        });
  }

  asio::io_context ioc_;
  tcp::endpoint endpoint_;
  tcp::acceptor acceptor_;
  asio::executor_work_guard<asio::io_context::executor_type> work_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Response> responses_;
  std::atomic_int active_{0};
  std::atomic_int max_active_{0};
  std::atomic_int identity_requests_{0};
};

class ImageUrlValidatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.start();
    config_.bridge_files_dir = "/tmp/bridge_files";
    config_.image_url_probe_timeout_ms = 1000;
  }

  MediaServer server_;
  BridgeConfig config_;
  asio::io_context ioc_;
};

TEST_F(ImageUrlValidatorTest, DownloadsImageBetweenEightAndTenMiB) {
  constexpr std::size_t kNineMiB = 9U * 1024U * 1024U;
  server_.set("/large", png_body(kNineMiB));

  auto results = run_awaitable(
      ioc_,
      ImageUrlValidator::download(config_, {{"photo", server_.url("/large")}}));

  ASSERT_EQ(results.size(), 1U);
  ASSERT_TRUE(results[0].succeeded());
  EXPECT_EQ(results[0].image->data.size(), kNineMiB);
  EXPECT_EQ(results[0].image->mime_type, "image/png");
  EXPECT_EQ(server_.identity_requests(), 1);
}

TEST_F(ImageUrlValidatorTest, ClassifiesKnownAndChunkedBodiesOverLimit) {
  config_.qq_media_download_max_bytes = 1024;
  server_.set("/known", png_body(1025));
  server_.set("/chunked", png_body(1025), true);

  auto results =
      run_awaitable(ioc_, ImageUrlValidator::download(
                              config_, {{"photo", server_.url("/known")},
                                        {"photo", server_.url("/chunked")}}));

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].failure, MediaDownloadFailure::OverLimit);
  EXPECT_EQ(results[1].failure, MediaDownloadFailure::OverLimit);
  EXPECT_EQ(results[0].diagnostic, "response exceeds configured media limit");
}

TEST_F(ImageUrlValidatorTest, PreservesOrderAndLimitsConcurrencyToThree) {
  std::vector<std::pair<std::string, std::string>> media;
  for (std::size_t index = 0; index < 7; ++index) {
    const auto path = "/delayed/" + std::to_string(index);
    server_.set(path, png_body(128), false,
                std::chrono::milliseconds(80 - static_cast<int>(index) * 5));
    media.emplace_back("photo", server_.url(path));
  }

  auto results =
      run_awaitable(ioc_, ImageUrlValidator::download(config_, media));

  ASSERT_EQ(results.size(), media.size());
  for (std::size_t index = 0; index < results.size(); ++index) {
    ASSERT_TRUE(results[index].succeeded());
    EXPECT_EQ(results[index].image->original_url, media[index].second);
  }
  EXPECT_EQ(server_.max_active(), 3);
  EXPECT_EQ(server_.active(), 0);
}

TEST_F(ImageUrlValidatorTest, CancellationJoinsDownloadWorkers) {
  std::vector<std::pair<std::string, std::string>> media;
  for (std::size_t index = 0; index < 3; ++index) {
    const auto path = "/cancel/" + std::to_string(index);
    server_.set(path, png_body(128), false, 500ms);
    media.emplace_back("photo", server_.url(path));
  }

  asio::cancellation_signal cancellation;
  auto future = asio::co_spawn(
      ioc_, ImageUrlValidator::download(config_, media),
      asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
  asio::steady_timer timer(ioc_, 20ms);
  timer.async_wait([&](const boost::system::error_code &) {
    cancellation.emit(asio::cancellation_type::terminal);
  });
  ioc_.run();

  const auto results = future.get();
  ASSERT_EQ(results.size(), media.size());
  EXPECT_TRUE(std::ranges::all_of(results, [](const auto &result) {
    return result.failure == MediaDownloadFailure::Transport;
  }));
}

TEST_F(ImageUrlValidatorTest, RejectsEmptyAndUnrecognizedBodiesPerItem) {
  server_.set("/empty", "");
  server_.set("/invalid", "not an image");

  auto results =
      run_awaitable(ioc_, ImageUrlValidator::download(
                              config_, {{"photo", server_.url("/empty")},
                                        {"photo", server_.url("/invalid")}}));

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].failure, MediaDownloadFailure::EmptyBody);
  EXPECT_EQ(results[1].failure, MediaDownloadFailure::InvalidImage);
}

} // namespace

} // namespace bridge::qq::test

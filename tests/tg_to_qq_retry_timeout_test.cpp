#include "common/config_loader.hpp"
#include "common/logger.hpp"
#include "core/qq_bot.hpp"
#include "core/task_scheduler.hpp"
#include "core/tg_bot.hpp"
#include "database/manager.hpp"
#include "interfaces/plugin.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "telegram/adapter/protocol_adapter.hpp"
#include "tg_to_qq_plugin.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// NOLINTBEGIN

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {

constexpr std::string_view kTelegramToken = "TEST_TOKEN";
constexpr std::string_view kTelegramGroupId = "-100123456";
constexpr std::string_view kQQGroupId = "998877";
constexpr size_t kConcurrentMessageCount = 16;

auto wait_until(std::chrono::milliseconds timeout, auto predicate) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return predicate();
}

auto make_temp_path(std::string_view suffix) -> std::filesystem::path {
  auto name = "obcx_bridge_retry_" + std::to_string(::getpid()) + "_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              std::string(suffix);
  return std::filesystem::temp_directory_path() / name;
}

class MockHttpServer {
public:
  explicit MockHttpServer(tcp::endpoint endpoint)
      : acceptor_(ioc_, endpoint), work_guard_(asio::make_work_guard(ioc_)) {
    acceptor_.set_option(asio::socket_base::reuse_address(true));
  }

  virtual ~MockHttpServer() { stop(); }

  void start() {
    thread_ = std::thread([this]() {
      do_accept();
      ioc_.run();
    });
  }

  void stop() {
    asio::post(ioc_, [this]() {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      {
        std::lock_guard lock(pending_sockets_mutex_);
        for (auto &socket : pending_sockets_) {
          if (socket && socket->is_open()) {
            socket->close(ignored);
          }
        }
        pending_sockets_.clear();
      }
      work_guard_.reset();
      ioc_.stop();
    });

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] auto port() const -> uint16_t {
    return acceptor_.local_endpoint().port();
  }

protected:
  virtual void handle_request(
      const std::shared_ptr<tcp::socket> &socket,
      const std::shared_ptr<http::request<http::string_body>> &request) = 0;

  void send_json(const std::shared_ptr<tcp::socket> &socket, std::string body) {
    auto response = std::make_shared<http::response<http::string_body>>(
        http::status::ok, 11);
    response->set(http::field::server, "OBCXBridgeMock/1.0");
    response->set(http::field::content_type, "application/json");
    response->body() = std::move(body);
    response->prepare_payload();

    http::async_write(*socket, *response,
                      [socket, response](beast::error_code, std::size_t) {
                        boost::system::error_code ignored;
                        socket->shutdown(tcp::socket::shutdown_both, ignored);
                        socket->close(ignored);
                      });
  }

  void keep_socket_open(const std::shared_ptr<tcp::socket> &socket) {
    std::lock_guard lock(pending_sockets_mutex_);
    pending_sockets_.push_back(socket);
  }

private:
  void do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
      if (!acceptor_.is_open()) {
        return;
      }
      if (!ec) {
        read_request(std::make_shared<tcp::socket>(std::move(socket)));
      }
      do_accept();
    });
  }

  void read_request(const std::shared_ptr<tcp::socket> &socket) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto request = std::make_shared<http::request<http::string_body>>();

    http::async_read(
        *socket, *buffer, *request,
        [this, socket, buffer, request](beast::error_code ec, std::size_t) {
          if (!ec) {
            handle_request(socket, request);
          }
        });
  }

  asio::io_context ioc_;
  tcp::acceptor acceptor_;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
  std::thread thread_;
  std::mutex pending_sockets_mutex_;
  std::vector<std::shared_ptr<tcp::socket>> pending_sockets_;
};

class MockTelegramServer final : public MockHttpServer {
public:
  MockTelegramServer()
      : MockHttpServer({asio::ip::make_address("127.0.0.1"), 0}) {}

  [[nodiscard]] auto get_updates_count() const -> size_t {
    return get_updates_count_.load();
  }

protected:
  void handle_request(const std::shared_ptr<tcp::socket> &socket,
                      const std::shared_ptr<http::request<http::string_body>>
                          &request) override {
    const std::string target = std::string(request->target());
    if (target.find("/getUpdates") == std::string::npos) {
      send_json(socket, R"({"ok":true,"result":true})");
      return;
    }

    const auto request_index = get_updates_count_.fetch_add(1);
    if (request_index > 0) {
      send_json(socket, R"({"ok":true,"result":[]})");
      return;
    }

    nlohmann::json updates = nlohmann::json::array();
    for (size_t i = 0; i < kConcurrentMessageCount; ++i) {
      updates.push_back({
          {"update_id", 1000 + static_cast<int>(i)},
          {"message",
           {{"message_id", 2000 + static_cast<int>(i)},
            {"date", 1700000000},
            {"chat",
             {{"id", std::stoll(std::string(kTelegramGroupId))},
              {"type", "supergroup"},
              {"title", "bridge-test"}}},
            {"from",
             {{"id", 3000 + static_cast<int>(i)},
              {"is_bot", false},
              {"first_name", "LoadTest"}}},
            {"text", "burst message " + std::to_string(i)}}},
      });
    }

    send_json(socket,
              nlohmann::json({{"ok", true}, {"result", updates}}).dump());
  }

private:
  std::atomic<size_t> get_updates_count_{0};
};

class MockQQServer final : public MockHttpServer {
public:
  MockQQServer() : MockHttpServer({asio::ip::make_address("127.0.0.1"), 0}) {}

  [[nodiscard]] auto send_group_request_count() const -> size_t {
    return send_group_request_count_.load();
  }

protected:
  void handle_request(const std::shared_ptr<tcp::socket> &socket,
                      const std::shared_ptr<http::request<http::string_body>>
                          &request) override {
    if (request->method() == http::verb::get) {
      send_json(socket, "[]");
      return;
    }

    try {
      const auto body = nlohmann::json::parse(request->body());
      if (body.value("action", "") == "send_group_msg") {
        send_group_request_count_.fetch_add(1);
        keep_socket_open(socket);
        return;
      }
    } catch (...) {
    }

    send_json(socket, R"({"status":"ok","data":{}})");
  }

private:
  std::atomic<size_t> send_group_request_count_{0};
};

class BridgeTGToQQRetryTimeoutTest : public testing::Test {
protected:
  void SetUp() override {
    obcx::common::Logger::initialize(spdlog::level::warn);

    telegram_server_ = std::make_unique<MockTelegramServer>();
    qq_server_ = std::make_unique<MockQQServer>();
    telegram_server_->start();
    qq_server_->start();

    db_path_ = make_temp_path(".db");
    config_path_ = make_temp_path(".toml");

    std::ofstream config_file(config_path_);
    config_file << "[bots.telegram_bot.connection]\n";
    config_file << "access_token = \"" << kTelegramToken << "\"\n";
    config_file << "host = \"127.0.0.1\"\n";
    config_file << "port = " << telegram_server_->port() << "\n";
    config_file << "\n[bots.qq_bot.connection]\n";
    config_file << "host = \"127.0.0.1\"\n";
    config_file << "port = " << qq_server_->port() << "\n";
    config_file << "\n[plugins.tg_to_qq]\n";
    config_file << "enabled = true\n";
    config_file << "\n[plugins.tg_to_qq.config]\n";
    config_file << "database_file = \"" << db_path_.string() << "\"\n";
    config_file << "enable_retry_queue = true\n";
    config_file << "bridge_files_dir = \""
                << std::filesystem::temp_directory_path().string() << "\"\n";
    config_file << "bridge_files_container_dir = \"/tmp\"\n";
    config_file << "\n[group_mappings]\n";
    config_file << "group_to_group = [\n";
    config_file << "  { telegram_group_id = \"" << kTelegramGroupId
                << "\", qq_group_id = \"" << kQQGroupId
                << "\", show_tg_to_qq_sender = true,"
                   " enable_tg_to_qq = true },\n";
    config_file << "]\n";
    config_file.close();

    ASSERT_TRUE(
        obcx::common::ConfigLoader::instance().load_config(config_path_));

    scheduler_ = std::make_shared<obcx::core::TaskScheduler>(16);

    auto telegram_bot = std::make_unique<obcx::core::TGBot>(
        obcx::adapter::telegram::ProtocolAdapter{}, scheduler_);
    auto qq_bot = std::make_unique<obcx::core::QQBot>(
        obcx::adapter::onebot11::ProtocolAdapter{}, scheduler_);

    telegram_bot_ = telegram_bot.get();
    qq_bot_ = qq_bot.get();

    obcx::common::ConnectionConfig telegram_config;
    telegram_config.host = "127.0.0.1";
    telegram_config.port = telegram_server_->port();
    telegram_config.access_token = std::string(kTelegramToken);
    telegram_config.connect_timeout = std::chrono::milliseconds(80);
    telegram_config.poll_timeout = std::chrono::milliseconds(1);
    telegram_config.poll_force_close = std::chrono::milliseconds(80);
    telegram_config.poll_retry_interval = std::chrono::milliseconds(20);
    telegram_config.use_ssl = false;

    obcx::common::ConnectionConfig qq_config;
    qq_config.host = "127.0.0.1";
    qq_config.port = qq_server_->port();
    qq_config.connect_timeout = std::chrono::milliseconds(80);
    qq_config.use_ssl = false;

    telegram_bot_->connect(
        obcx::network::ConnectionManagerFactory::ConnectionType::TelegramHTTP,
        telegram_config);
    qq_bot_->connect(
        obcx::network::ConnectionManagerFactory::ConnectionType::Onebot11HTTP,
        qq_config);

    bots_.push_back(std::move(telegram_bot));
    bots_.push_back(std::move(qq_bot));
    obcx::interface::IPlugin::set_bots(&bots_, &bots_mutex_);

    plugin_ = std::make_unique<plugins::TGToQQPlugin>();
    ASSERT_TRUE(plugin_->initialize());

    telegram_thread_ = std::thread([this]() { telegram_bot_->run(); });
    qq_thread_ = std::thread([this]() { qq_bot_->run(); });
  }

  void TearDown() override {
    if (plugin_) {
      plugin_->shutdown();
      plugin_.reset();
    }

    if (telegram_bot_) {
      telegram_bot_->stop();
    }
    if (qq_bot_) {
      qq_bot_->stop();
    }

    if (telegram_thread_.joinable()) {
      telegram_thread_.join();
    }
    if (qq_thread_.joinable()) {
      qq_thread_.join();
    }

    bots_.clear();
    obcx::interface::IPlugin::set_bots(nullptr, nullptr);

    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }

    storage::DatabaseManager::reset_instance();

    if (telegram_server_) {
      telegram_server_->stop();
    }
    if (qq_server_) {
      qq_server_->stop();
    }

    std::error_code ignored;
    std::filesystem::remove(db_path_, ignored);
    std::filesystem::remove(config_path_, ignored);
  }

  std::unique_ptr<MockTelegramServer> telegram_server_;
  std::unique_ptr<MockQQServer> qq_server_;
  std::filesystem::path db_path_;
  std::filesystem::path config_path_;
  std::shared_ptr<obcx::core::TaskScheduler> scheduler_;
  std::vector<std::unique_ptr<obcx::core::IBot>> bots_;
  std::mutex bots_mutex_;
  obcx::core::TGBot *telegram_bot_{nullptr};
  obcx::core::QQBot *qq_bot_{nullptr};
  std::unique_ptr<plugins::TGToQQPlugin> plugin_;
  std::thread telegram_thread_;
  std::thread qq_thread_;
};

TEST_F(BridgeTGToQQRetryTimeoutTest,
       ConcurrentTelegramBurstToUnresponsiveQQRetriesWithoutCrash) {
  ASSERT_TRUE(wait_until(std::chrono::seconds(5), [this]() {
    return telegram_server_->get_updates_count() > 0 &&
           qq_server_->send_group_request_count() >= kConcurrentMessageCount;
  }));

  ASSERT_TRUE(wait_until(std::chrono::seconds(15), [this]() {
    return qq_server_->send_group_request_count() >=
           kConcurrentMessageCount + 1;
  })) << "retry queue did not issue a QQ retry request";

  plugin_->shutdown();
  plugin_.reset();
}

} // namespace

// NOLINTEND

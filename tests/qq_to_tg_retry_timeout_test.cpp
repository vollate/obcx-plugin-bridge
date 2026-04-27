#include "common/config_loader.hpp"
#include "common/logger.hpp"
#include "core/qq_bot.hpp"
#include "core/task_scheduler.hpp"
#include "core/tg_bot.hpp"
#include "database/manager.hpp"
#include "interfaces/plugin.hpp"
#include "onebot11/adapter/protocol_adapter.hpp"
#include "qq_to_tg_plugin.hpp"
#include "telegram/adapter/protocol_adapter.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

// NOLINTBEGIN

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {

constexpr std::string_view kTelegramToken = "TEST_TOKEN";
constexpr std::string_view kTelegramGroupId = "-114514";
constexpr std::string_view kQQGroupId = "2222";

struct StressConfig {
  size_t messages_per_second{10000};
  std::chrono::seconds duration{60};
  size_t qq_batch_size{10000};
  double tg_outage_probability_per_second{0.2};
  std::chrono::milliseconds tg_outage_duration{1000};
  std::chrono::milliseconds tg_timeout{80};
};

auto get_env_size(const char *name, size_t fallback) -> size_t {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  try {
    return static_cast<size_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

auto get_env_double(const char *name, double fallback) -> double {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  try {
    return std::stod(value);
  } catch (...) {
    return fallback;
  }
}

auto load_stress_config() -> StressConfig {
  StressConfig config;
  config.messages_per_second = get_env_size(
      "OBCX_BRIDGE_STRESS_MESSAGES_PER_SECOND", config.messages_per_second);
  config.duration = std::chrono::seconds(
      get_env_size("OBCX_BRIDGE_STRESS_DURATION_SECONDS",
                   static_cast<size_t>(config.duration.count())));
  config.qq_batch_size =
      get_env_size("OBCX_BRIDGE_STRESS_QQ_BATCH_SIZE", config.qq_batch_size);
  config.tg_outage_probability_per_second =
      get_env_double("OBCX_BRIDGE_STRESS_TG_OUTAGE_PROBABILITY",
                     config.tg_outage_probability_per_second);
  config.tg_outage_duration = std::chrono::milliseconds(
      get_env_size("OBCX_BRIDGE_STRESS_TG_OUTAGE_MS",
                   static_cast<size_t>(config.tg_outage_duration.count())));
  config.tg_timeout = std::chrono::milliseconds(
      get_env_size("OBCX_BRIDGE_STRESS_TG_TIMEOUT_MS",
                   static_cast<size_t>(config.tg_timeout.count())));
  return config;
}

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
  auto name = "obcx_bridge_qq_to_tg_retry_" + std::to_string(::getpid()) + "_" +
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

  void keep_socket_open_for(const std::shared_ptr<tcp::socket> &socket,
                            std::chrono::milliseconds duration) {
    auto timer =
        std::make_shared<asio::steady_timer>(socket->get_executor(), duration);
    timer->async_wait([socket, timer](beast::error_code) {
      boost::system::error_code ignored;
      socket->close(ignored);
    });
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
};

class MockQQEventServer final : public MockHttpServer {
public:
  explicit MockQQEventServer(StressConfig config)
      : MockHttpServer({asio::ip::make_address("127.0.0.1"), 0}),
        config_(config),
        total_message_count_(config_.messages_per_second *
                             static_cast<size_t>(config_.duration.count())) {}

  [[nodiscard]] auto poll_count() const -> size_t { return poll_count_.load(); }

  [[nodiscard]] auto emitted_message_count() const -> size_t {
    return emitted_message_count_.load();
  }

  [[nodiscard]] auto generated_message_count() const -> size_t {
    const auto elapsed = std::chrono::steady_clock::now() - started_at_;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    return std::min(
        total_message_count_,
        static_cast<size_t>((elapsed_ms * config_.messages_per_second) / 1000));
  }

  [[nodiscard]] auto total_message_count() const -> size_t {
    return total_message_count_;
  }

protected:
  void handle_request(const std::shared_ptr<tcp::socket> &socket,
                      const std::shared_ptr<http::request<http::string_body>>
                          &request) override {
    const std::string target = std::string(request->target());
    if (request->method() == http::verb::get &&
        target.find("/get_latest_events") != std::string::npos) {
      handle_event_poll(socket);
      return;
    }

    if (request->method() == http::verb::post) {
      handle_action(socket, request->body());
      return;
    }

    send_json(socket, "[]");
  }

private:
  void handle_event_poll(const std::shared_ptr<tcp::socket> &socket) {
    poll_count_.fetch_add(1);

    const auto target_count = generated_message_count();
    const auto already_emitted = emitted_message_count_.load();
    if (already_emitted >= target_count) {
      send_json(socket, "[]");
      return;
    }

    const auto batch_count =
        std::min(config_.qq_batch_size, target_count - already_emitted);
    nlohmann::json events = nlohmann::json::array();
    for (size_t i = 0; i < batch_count; ++i) {
      const auto message_index = already_emitted + i;
      const auto text = "stream message " + std::to_string(message_index);
      events.push_back({
          {"time", 1700000000},
          {"self_id", 10000},
          {"post_type", "message"},
          {"message_type", "group"},
          {"sub_type", "normal"},
          {"message_id", std::to_string(2000000 + message_index)},
          {"group_id", std::string(kQQGroupId)},
          {"user_id", std::to_string(3000000 + message_index)},
          {"raw_message", text},
          {"font", 0},
          {"sender",
           {{"user_id", std::to_string(3000000 + message_index)},
            {"nickname", "LoadTest"},
            {"card", "LoadTest"}}},
          {"message", nlohmann::json::array(
                          {{{"type", "text"}, {"data", {{"text", text}}}}})},
      });
    }
    emitted_message_count_.fetch_add(batch_count);
    send_json(socket, events.dump());
  }

  void handle_action(const std::shared_ptr<tcp::socket> &socket,
                     const std::string &body) {
    try {
      const auto request = nlohmann::json::parse(body);
      const auto action = request.value("action", "");
      if (action == "get_group_member_info") {
        send_json(
            socket,
            R"({"status":"ok","data":{"nickname":"LoadTest","card":"LoadTest","title":""}})");
        return;
      }
    } catch (...) {
    }

    send_json(socket, R"({"status":"ok","data":{}})");
  }

  StressConfig config_;
  size_t total_message_count_;
  std::chrono::steady_clock::time_point started_at_{
      std::chrono::steady_clock::now()};
  std::atomic<size_t> poll_count_{0};
  std::atomic<size_t> emitted_message_count_{0};
};

class MockTelegramSendServer final : public MockHttpServer {
public:
  explicit MockTelegramSendServer(StressConfig config)
      : MockHttpServer({asio::ip::make_address("127.0.0.1"), 0}),
        config_(config) {}

  [[nodiscard]] auto send_message_count() const -> size_t {
    return send_message_count_.load();
  }

  [[nodiscard]] auto timed_out_request_count() const -> size_t {
    return timed_out_request_count_.load();
  }

  [[nodiscard]] auto duplicate_request_count() const -> size_t {
    return duplicate_request_count_.load();
  }

  [[nodiscard]] auto outage_count() const -> size_t {
    return outage_count_.load();
  }

protected:
  void handle_request(const std::shared_ptr<tcp::socket> &socket,
                      const std::shared_ptr<http::request<http::string_body>>
                          &request) override {
    const std::string target = std::string(request->target());
    if (target.find("/getUpdates") != std::string::npos) {
      send_json(socket, R"({"ok":true,"result":[]})");
      return;
    }

    if (target.find("/sendMessage") == std::string::npos) {
      send_json(socket, R"({"ok":true,"result":true})");
      return;
    }

    send_message_count_.fetch_add(1);
    try {
      record_message_identity(nlohmann::json::parse(request->body()));
    } catch (...) {
    }

    if (is_in_outage()) {
      timed_out_request_count_.fetch_add(1);
      keep_socket_open_for(socket, config_.tg_timeout * 3);
      return;
    }

    const auto message_id = successful_request_count_.fetch_add(1) + 1;
    send_json(
        socket,
        nlohmann::json(
            {{"ok", true},
             {"result", {{"message_id", static_cast<int64_t>(message_id)}}}})
            .dump());
  }

private:
  auto is_in_outage() -> bool {
    const auto now = std::chrono::steady_clock::now();
    if (now < outage_until_) {
      return true;
    }

    if (outage_count_.load() == 0 && send_message_count_.load() >= 1) {
      outage_until_ = now + config_.tg_outage_duration;
      outage_count_.fetch_add(1);
      return true;
    }

    if (now >= next_outage_decision_at_) {
      next_outage_decision_at_ = now + std::chrono::seconds(1);
      if (outage_distribution_(rng_) <
          config_.tg_outage_probability_per_second) {
        outage_until_ = now + config_.tg_outage_duration;
        outage_count_.fetch_add(1);
        return true;
      }
    }

    return false;
  }

  void record_message_identity(const nlohmann::json &body) {
    std::string identity;
    if (!find_stream_message_identity(body, identity)) {
      return;
    }

    std::lock_guard lock(seen_message_mutex_);
    if (!seen_messages_.insert(identity).second) {
      duplicate_request_count_.fetch_add(1);
    }
  }

  auto find_stream_message_identity(const nlohmann::json &value,
                                    std::string &identity) -> bool {
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      const auto marker = text.find("stream message ");
      if (marker == std::string::npos) {
        return false;
      }

      auto end = marker + std::string("stream message ").size();
      while (end < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
        ++end;
      }
      identity = text.substr(marker, end - marker);
      return true;
    }

    if (value.is_array()) {
      for (const auto &item : value) {
        if (find_stream_message_identity(item, identity)) {
          return true;
        }
      }
      return false;
    }

    if (value.is_object()) {
      for (const auto &item : value.items()) {
        if (find_stream_message_identity(item.value(), identity)) {
          return true;
        }
      }
    }

    return false;
  }

  StressConfig config_;
  std::mt19937 rng_{0x0BCB};
  std::uniform_real_distribution<double> outage_distribution_{0.0, 1.0};
  std::chrono::steady_clock::time_point next_outage_decision_at_{
      std::chrono::steady_clock::now()};
  std::chrono::steady_clock::time_point outage_until_{
      std::chrono::steady_clock::time_point::min()};
  std::mutex seen_message_mutex_;
  std::unordered_set<std::string> seen_messages_;
  std::atomic<size_t> send_message_count_{0};
  std::atomic<size_t> successful_request_count_{0};
  std::atomic<size_t> timed_out_request_count_{0};
  std::atomic<size_t> duplicate_request_count_{0};
  std::atomic<size_t> outage_count_{0};
};

class BridgeQQToTGRetryTimeoutTest : public testing::Test {
protected:
  void SetUp() override {
    obcx::common::Logger::initialize(spdlog::level::warn);
    stress_config_ = load_stress_config();

    qq_server_ = std::make_unique<MockQQEventServer>(stress_config_);
    telegram_server_ = std::make_unique<MockTelegramSendServer>(stress_config_);
    qq_server_->start();
    telegram_server_->start();

    db_path_ = make_temp_path(".db");
    config_path_ = make_temp_path(".toml");

    std::ofstream config_file(config_path_);
    config_file << "[bots.qq_bot.connection]\n";
    config_file << "host = \"127.0.0.1\"\n";
    config_file << "port = " << qq_server_->port() << "\n";
    config_file << "\n[bots.telegram_bot.connection]\n";
    config_file << "access_token = \"" << kTelegramToken << "\"\n";
    config_file << "host = \"127.0.0.1\"\n";
    config_file << "port = " << telegram_server_->port() << "\n";
    config_file << "\n[plugins.qq_to_tg]\n";
    config_file << "enabled = true\n";
    config_file << "\n[plugins.qq_to_tg.config]\n";
    config_file << "database_file = \"" << db_path_.string() << "\"\n";
    config_file << "enable_retry_queue = true\n";
    config_file << "bridge_files_dir = \""
                << std::filesystem::temp_directory_path().string() << "\"\n";
    config_file << "bridge_files_container_dir = \"/tmp\"\n";
    config_file << "\n[group_mappings]\n";
    config_file << "group_to_group = [\n";
    config_file << "  { telegram_group_id = \"" << kTelegramGroupId
                << "\", qq_group_id = \"" << kQQGroupId
                << "\", show_qq_to_tg_sender = true,"
                   " enable_qq_to_tg = true },\n";
    config_file << "]\n";
    config_file.close();

    ASSERT_TRUE(
        obcx::common::ConfigLoader::instance().load_config(config_path_));

    scheduler_ = std::make_shared<obcx::core::TaskScheduler>(16);

    auto qq_bot = std::make_unique<obcx::core::QQBot>(
        obcx::adapter::onebot11::ProtocolAdapter{}, scheduler_);
    auto telegram_bot = std::make_unique<obcx::core::TGBot>(
        obcx::adapter::telegram::ProtocolAdapter{}, scheduler_);

    qq_bot_ = qq_bot.get();
    telegram_bot_ = telegram_bot.get();

    obcx::common::ConnectionConfig qq_config;
    qq_config.host = "127.0.0.1";
    qq_config.port = qq_server_->port();
    qq_config.connect_timeout = std::chrono::milliseconds(80);
    qq_config.use_ssl = false;

    obcx::common::ConnectionConfig telegram_config;
    telegram_config.host = "127.0.0.1";
    telegram_config.port = telegram_server_->port();
    telegram_config.access_token = std::string(kTelegramToken);
    telegram_config.connect_timeout = stress_config_.tg_timeout;
    telegram_config.poll_timeout = std::chrono::milliseconds(1);
    telegram_config.poll_force_close = stress_config_.tg_timeout;
    telegram_config.poll_retry_interval = std::chrono::milliseconds(20);
    telegram_config.use_ssl = false;

    qq_bot_->connect(
        obcx::network::ConnectionManagerFactory::ConnectionType::Onebot11HTTP,
        qq_config);
    telegram_bot_->connect(
        obcx::network::ConnectionManagerFactory::ConnectionType::TelegramHTTP,
        telegram_config);

    bots_.push_back(std::move(qq_bot));
    bots_.push_back(std::move(telegram_bot));
    obcx::interface::IPlugin::set_bots(&bots_, &bots_mutex_);

    plugin_ = std::make_unique<plugins::QQToTGPlugin>();
    ASSERT_TRUE(plugin_->initialize());

    qq_thread_ = std::thread([this]() { qq_bot_->run(); });
    telegram_thread_ = std::thread([this]() { telegram_bot_->run(); });
  }

  void TearDown() override {
    if (plugin_) {
      plugin_->shutdown();
      plugin_.reset();
    }

    if (qq_bot_) {
      qq_bot_->stop();
    }
    if (telegram_bot_) {
      telegram_bot_->stop();
    }

    if (qq_thread_.joinable()) {
      qq_thread_.join();
    }
    if (telegram_thread_.joinable()) {
      telegram_thread_.join();
    }

    bots_.clear();
    obcx::interface::IPlugin::set_bots(nullptr, nullptr);

    if (scheduler_) {
      scheduler_->stop();
      scheduler_.reset();
    }

    storage::DatabaseManager::reset_instance();

    if (qq_server_) {
      qq_server_->stop();
    }
    if (telegram_server_) {
      telegram_server_->stop();
    }

    std::error_code ignored;
    std::filesystem::remove(db_path_, ignored);
    std::filesystem::remove(config_path_, ignored);
  }

  std::unique_ptr<MockQQEventServer> qq_server_;
  std::unique_ptr<MockTelegramSendServer> telegram_server_;
  StressConfig stress_config_;
  std::filesystem::path db_path_;
  std::filesystem::path config_path_;
  std::shared_ptr<obcx::core::TaskScheduler> scheduler_;
  std::vector<std::unique_ptr<obcx::core::IBot>> bots_;
  std::mutex bots_mutex_;
  obcx::core::QQBot *qq_bot_{nullptr};
  obcx::core::TGBot *telegram_bot_{nullptr};
  std::unique_ptr<plugins::QQToTGPlugin> plugin_;
  std::thread qq_thread_;
  std::thread telegram_thread_;
};

TEST_F(BridgeQQToTGRetryTimeoutTest,
       ConcurrentQQStreamToFlakyTelegramRetriesWithoutCrash) {
  ASSERT_TRUE(wait_until(std::chrono::seconds(5), [this]() {
    return qq_server_->poll_count() > 0;
  })) << "QQ bot did not start polling the mock OneBot server";

  std::this_thread::sleep_for(stress_config_.duration);

  ASSERT_GE(qq_server_->generated_message_count(),
            qq_server_->total_message_count())
      << "QQ mock did not run for the configured stress duration";
  ASSERT_GT(qq_server_->emitted_message_count(), 0U)
      << "QQ mock did not deliver any events to the bot";
  ASSERT_GT(telegram_server_->send_message_count(), 0U)
      << "Telegram mock did not receive any forwarded messages";

  ASSERT_GT(telegram_server_->outage_count(), 0U)
      << "Telegram mock never entered an outage window";
  ASSERT_GT(telegram_server_->timed_out_request_count(), 0U)
      << "Telegram outage windows did not force any send timeouts";

  ASSERT_TRUE(wait_until(std::chrono::seconds(30), [this]() {
    return telegram_server_->duplicate_request_count() > 0;
  })) << "retry queue did not retry any timed-out Telegram sends";

  plugin_->shutdown();
  plugin_.reset();
}

} // namespace

// NOLINTEND

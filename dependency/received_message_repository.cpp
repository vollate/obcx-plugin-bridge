#include "received_message_repository.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bridge {
namespace {

auto sanitize_identifier_part(const std::string &value) -> std::string {
  std::string sanitized;
  sanitized.reserve(value.size());
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (std::isalnum(byte)) {
      sanitized.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      continue;
    }
    if (ch == '_') {
      sanitized.push_back('_');
      continue;
    }
    if (!sanitized.empty() && sanitized.back() != '_') {
      sanitized.push_back('_');
    }
  }
  while (!sanitized.empty() && sanitized.back() == '_') {
    sanitized.pop_back();
  }
  if (sanitized.empty()) {
    throw std::invalid_argument("message_store namespace/platform is empty");
  }
  return sanitized;
}

auto quote_identifier(const std::string &identifier) -> std::string {
  return "\"" + identifier + "\"";
}

auto table_name(const std::string &db_namespace, const std::string &platform)
    -> std::string {
  return sanitize_identifier_part(db_namespace) + "_" +
         sanitize_identifier_part(platform) + "_messages";
}

auto table_exists(obcx::core::IDbConnection &connection,
                  const std::string &name) -> bool {
  const auto rows = connection.query(
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name = ?;",
      {name});
  return !rows.empty();
}

auto db_string(const obcx::core::DbRow &row, const std::string &key)
    -> std::string {
  const auto it = row.find(key);
  if (it == row.end()) {
    return {};
  }

  return std::visit(
      [](const auto &value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::string>) {
          return value;
        } else if constexpr (std::is_same_v<Value, std::int64_t>) {
          return std::to_string(value);
        } else if constexpr (std::is_same_v<Value, double>) {
          return std::to_string(value);
        } else {
          return {};
        }
      },
      it->second);
}

auto db_int64(const obcx::core::DbRow &row, const std::string &key)
    -> std::int64_t {
  const auto it = row.find(key);
  if (it == row.end()) {
    return 0;
  }

  return std::visit(
      [](const auto &value) -> std::int64_t {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::int64_t>) {
          return value;
        } else if constexpr (std::is_same_v<Value, double>) {
          return static_cast<std::int64_t>(value);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          return value.empty() ? 0 : std::stoll(value);
        } else {
          return 0;
        }
      },
      it->second);
}

auto parse_json_or_object(const std::string &text) -> nlohmann::json {
  auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded()) {
    return nlohmann::json::object();
  }
  return parsed;
}

auto json_text(const nlohmann::json &value, const std::string &key)
    -> std::string {
  if (!value.is_object() || !value.contains(key) || value.at(key).is_null()) {
    return {};
  }
  const auto &item = value.at(key);
  if (item.is_string()) {
    return item.get<std::string>();
  }
  if (item.is_number_integer()) {
    return std::to_string(item.get<std::int64_t>());
  }
  if (item.is_number_unsigned()) {
    return std::to_string(item.get<std::uint64_t>());
  }
  return {};
}

auto extract_content(const nlohmann::json &payload) -> std::string {
  auto text = json_text(payload, "text");
  if (!text.empty()) {
    return text;
  }
  text = json_text(payload, "raw_message");
  if (!text.empty()) {
    return text;
  }
  if (payload.contains("message") && payload["message"].is_array()) {
    std::string combined;
    for (const auto &segment : payload["message"]) {
      if (!segment.is_object() || !segment.contains("type")) {
        continue;
      }
      if (segment.value("type", std::string{}) == "text" &&
          segment.contains("data")) {
        combined += json_text(segment["data"], "text");
      }
    }
    return combined;
  }
  return {};
}

auto from_unix_millis(const std::int64_t millis)
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point{
      std::chrono::milliseconds{millis}};
}

} // namespace

ReceivedMessageRepository::ReceivedMessageRepository(
    obcx::core::DbManager &db_manager, std::string db_instance,
    std::string db_namespace)
    : db_manager_(db_manager), db_instance_(std::move(db_instance)),
      db_namespace_(std::move(db_namespace)) {
  if (db_instance_.empty()) {
    db_instance_ = "main";
  }
  if (db_namespace_.empty()) {
    db_namespace_ = "message_store";
  }
}

auto ReceivedMessageRepository::get_message(const std::string &source_platform,
                                            const std::string &source_bot,
                                            const std::string &conversation_id,
                                            const std::string &message_id) const
    -> std::optional<storage::MessageInfo> {
  if (source_platform.empty() || source_bot.empty() ||
      conversation_id.empty() || message_id.empty()) {
    return std::nullopt;
  }

  return db_manager_.run_read<std::optional<storage::MessageInfo>>(
      db_instance_,
      [&](obcx::core::IDbConnection &connection)
          -> std::optional<storage::MessageInfo> {
        const auto table = table_name(db_namespace_, source_platform);
        if (!table_exists(connection, table)) {
          return std::nullopt;
        }

        const auto rows = connection.query(
            "SELECT message_id, source_platform, source_bot, conversation_id,"
            " sender, group_id, message_type, payload, raw, timestamp FROM " +
                quote_identifier(table) +
                " WHERE source_platform = ? AND source_bot = ? AND"
                " conversation_id = ? AND message_id = ? LIMIT 1;",
            {source_platform, source_bot, conversation_id, message_id});
        if (rows.empty()) {
          return std::nullopt;
        }

        const auto &row = rows.front();
        const auto payload = parse_json_or_object(db_string(row, "payload"));
        storage::MessageInfo message;
        message.platform = db_string(row, "source_platform");
        message.source_bot = db_string(row, "source_bot");
        message.conversation_id = db_string(row, "conversation_id");
        message.message_id = db_string(row, "message_id");
        message.group_id = db_string(row, "group_id");
        message.user_id = db_string(row, "sender");
        message.message_type = db_string(row, "message_type");
        message.content = extract_content(payload);
        message.raw_message = db_string(row, "raw");
        message.timestamp = from_unix_millis(db_int64(row, "timestamp"));
        message.created_at = message.timestamp;

        const auto reply_id = json_text(payload, "reply_to_message_id");
        if (!reply_id.empty()) {
          message.reply_to_message_id = reply_id;
        }
        return message;
      });
}

} // namespace bridge

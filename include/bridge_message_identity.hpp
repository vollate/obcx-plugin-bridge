#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace bridge {

inline constexpr std::string_view qq_platform = "qq";
inline constexpr std::string_view telegram_platform = "telegram";
inline constexpr std::string_view qq_conversation_prefix = "group:";
inline constexpr std::string_view telegram_conversation_prefix = "chat:";

struct BridgeMessageScope {
  std::string installation_id;
  std::string platform;
  std::string conversation_id;

  auto operator==(const BridgeMessageScope &) const -> bool = default;
};

struct BridgeMessageIdentity {
  std::string installation_id;
  std::string platform;
  std::string conversation_id;
  std::string message_id;

  auto operator==(const BridgeMessageIdentity &) const -> bool = default;
};

[[nodiscard]] inline auto conversation_prefix(const std::string_view platform)
    -> std::string_view {
  if (platform == qq_platform) {
    return qq_conversation_prefix;
  }
  if (platform == telegram_platform) {
    return telegram_conversation_prefix;
  }
  return {};
}

[[nodiscard]] inline auto canonical_conversation_id(
    const std::string_view platform, const std::string_view native_id)
    -> std::string {
  const auto prefix = conversation_prefix(platform);
  if (prefix.empty()) {
    throw std::invalid_argument("unsupported Bridge message platform");
  }
  if (native_id.empty()) {
    throw std::invalid_argument("Bridge conversation native id is empty");
  }
  if (native_id.starts_with(prefix)) {
    return std::string{native_id};
  }
  return std::string{prefix} + std::string{native_id};
}

[[nodiscard]] inline auto qq_conversation_id(const std::string_view group_id)
    -> std::string {
  return canonical_conversation_id(qq_platform, group_id);
}

[[nodiscard]] inline auto telegram_conversation_id(
    const std::string_view chat_id) -> std::string {
  return canonical_conversation_id(telegram_platform, chat_id);
}

[[nodiscard]] inline auto valid_conversation_id(
    const std::string_view platform, const std::string_view conversation_id)
    -> bool {
  const auto prefix = conversation_prefix(platform);
  return !prefix.empty() && conversation_id.starts_with(prefix) &&
         conversation_id.size() > prefix.size();
}

[[nodiscard]] inline auto native_conversation_id(
    const std::string_view platform, const std::string_view conversation_id)
    -> std::string {
  if (!valid_conversation_id(platform, conversation_id)) {
    throw std::invalid_argument(
        "Bridge conversation id is incompatible with its platform");
  }
  return std::string{
      conversation_id.substr(conversation_prefix(platform).size())};
}

inline void validate_message_scope(const BridgeMessageScope &scope,
                                   const std::string_view field) {
  if (scope.installation_id.empty()) {
    throw std::invalid_argument(std::string{field} + " installation is empty");
  }
  if (!valid_conversation_id(scope.platform, scope.conversation_id)) {
    throw std::invalid_argument(std::string{field} +
                                " conversation is not canonical");
  }
}

inline void validate_message_identity(const BridgeMessageIdentity &identity,
                                      const std::string_view field) {
  validate_message_scope({.installation_id = identity.installation_id,
                          .platform = identity.platform,
                          .conversation_id = identity.conversation_id},
                         field);
  if (identity.message_id.empty()) {
    throw std::invalid_argument(std::string{field} + " message id is empty");
  }
}

} // namespace bridge

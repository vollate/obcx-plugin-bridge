#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bridge {

class BridgeSchemaMigrationRequiresRestart final : public std::runtime_error {
public:
  BridgeSchemaMigrationRequiresRestart()
      : std::runtime_error("bridge schema migration requires process restart") {
  }
};

enum class LegacyUnresolvedMappingPolicy {
  Fail,
  Archive,
};

struct LegacyConversationRoute {
  std::string pair_id;
  std::string telegram_installation;
  std::string onebot11_installation;
  std::string telegram_conversation_id;
  std::int64_t telegram_topic_id = -1;
  std::string qq_conversation_id;
};

struct BridgeStateMigrationContext {
  std::string pair_id;
  std::string telegram_installation;
  std::string onebot11_installation;
  std::vector<LegacyConversationRoute> conversation_routes;
  LegacyUnresolvedMappingPolicy unresolved_mapping_policy =
      LegacyUnresolvedMappingPolicy::Fail;
  std::string message_store_namespace = "message_store";
  bool allow_legacy_migration = true;
};

} // namespace bridge

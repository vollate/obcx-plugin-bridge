#include "bridge_db_runtime.hpp"

#include <utility>

namespace bridge {
namespace {

auto fallback_sqlite_config(const std::string &database_file)
    -> obcx::common::DbInstanceConfig {
  obcx::common::DbInstanceConfig config;
  config.name = "main";
  config.type = "sqlite";
  config.path = database_file.empty() ? "bridge_bot.db" : database_file;
  return config;
}

} // namespace

auto shared_core_db_manager(const std::string &database_file)
    -> std::shared_ptr<obcx::core::DbManager> {
  return obcx::core::DbManager::shared_manager(
      {fallback_sqlite_config(database_file)});
}

void reset_shared_db_managers_for_tests() {
  obcx::core::DbManager::reset_shared_managers_for_tests();
}

} // namespace bridge

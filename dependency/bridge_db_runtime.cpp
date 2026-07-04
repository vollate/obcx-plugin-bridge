#include "bridge_db_runtime.hpp"

#include <common/config_loader.hpp>

#include <algorithm>
#include <utility>
#include <vector>

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

auto has_main_instance(
    const std::vector<obcx::common::DbInstanceConfig> &configs) -> bool {
  return std::ranges::any_of(
      configs, [](const auto &config) { return config.name == "main"; });
}

} // namespace

auto shared_core_db_manager(const std::string &database_file)
    -> std::shared_ptr<obcx::core::DbManager> {
  auto configs =
      obcx::common::ConfigLoader::instance().get_db_instance_configs();
  if (configs.empty() || !has_main_instance(configs)) {
    configs = {fallback_sqlite_config(database_file)};
  }
  return obcx::core::DbManager::shared_manager(std::move(configs));
}

void reset_shared_db_managers_for_tests() {
  obcx::core::DbManager::reset_shared_managers_for_tests();
}

} // namespace bridge

#include "bridge_db_runtime.hpp"

#include <core/db_manager.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

auto temp_db_path(const std::string &name) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("obcx_bridge_db_runtime_" + name + "_" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          ".sqlite3");
}

} // namespace

TEST(BridgeDbRuntimeTest, ReusesCoreDbManagerForSameDatabaseFile) {
  bridge::reset_shared_db_managers_for_tests();
  const auto db_path = temp_db_path("shared");

  obcx::common::DbInstanceConfig config;
  config.name = "main";
  config.type = "sqlite";
  config.path = db_path.string();

  auto core = obcx::core::DbManager::shared_manager({config});
  auto first = bridge::shared_core_db_manager(db_path.string());
  auto second = bridge::shared_core_db_manager(db_path.string());

  ASSERT_NE(first, nullptr);
  ASSERT_EQ(first, core);
  ASSERT_EQ(first, second);

  first->run_write<void>("main", [](obcx::core::IDbConnection &connection) {
    connection.execute("CREATE TABLE probe (value TEXT NOT NULL);");
    connection.execute("INSERT INTO probe (value) VALUES (?);",
                       {std::string{"shared"}});
  });

  const auto value = second->run_read<std::string>(
      "main", [](obcx::core::IDbConnection &connection) {
        const auto rows = connection.query("SELECT value FROM probe LIMIT 1;");
        return std::get<std::string>(rows.front().at("value"));
      });

  EXPECT_EQ(value, "shared");
}

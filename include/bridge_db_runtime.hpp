#pragma once

#include <core/db_manager.hpp>

#include <memory>
#include <string>

namespace bridge {

[[nodiscard]] auto shared_core_db_manager(const std::string &database_file)
    -> std::shared_ptr<obcx::core::DbManager>;

void reset_shared_db_managers_for_tests();

} // namespace bridge

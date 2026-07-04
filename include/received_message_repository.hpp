#pragma once

#include "bridge_storage_models.hpp"

#include <core/db_manager.hpp>

#include <optional>
#include <string>

namespace bridge {

class ReceivedMessageRepository {
public:
  ReceivedMessageRepository(obcx::core::DbManager &db_manager,
                            std::string db_instance,
                            std::string db_namespace = "message_store");

  auto get_message(const std::string &source_platform,
                   const std::string &message_id) const
      -> std::optional<storage::MessageInfo>;

private:
  obcx::core::DbManager &db_manager_;
  std::string db_instance_;
  std::string db_namespace_;
};

} // namespace bridge

#include "database/manager.hpp"

#include <common/logger.hpp>
#include <utility>

namespace storage {

// === 单例管理 ===

std::shared_ptr<DatabaseManager> DatabaseManager::instance_ = nullptr;
std::mutex DatabaseManager::instance_mutex_;

auto DatabaseManager::instance(const std::string &db_path)
    -> std::shared_ptr<DatabaseManager> {
  std::lock_guard lock(instance_mutex_);
  if (!instance_) {
    if (db_path.empty()) {
      PLUGIN_ERROR("bridge",
                   "DatabaseManager::instance() called without db_path on "
                   "first initialization");
      return nullptr;
    }
    instance_ = std::shared_ptr<DatabaseManager>(new DatabaseManager(db_path));
  }
  return instance_;
}

void DatabaseManager::reset_instance() {
  std::lock_guard lock(instance_mutex_);
  instance_.reset();
  PLUGIN_DEBUG("bridge", "DatabaseManager instance reset");
}

// === 构造函数和析构函数 ===

DatabaseManager::DatabaseManager(std::string db_path)
    : db_path_(std::move(db_path)), db_(nullptr) {
  PLUGIN_DEBUG("bridge", "DatabaseManager constructed with path: {}", db_path_);
}

DatabaseManager::~DatabaseManager() {
  if (db_) {
    // Use sqlite3_close_v2 which handles pending operations gracefully
    // It will mark the connection as unusable and close it when all
    // pending operations complete
    sqlite3_close_v2(db_);
    PLUGIN_DEBUG("bridge", "Database closed");
  }
}

// === 初始化函数 ===

auto DatabaseManager::initialize() -> bool {
  std::lock_guard lock(db_mutex_);

  // 已经初始化过则直接返回
  if (initialized_ && db_) {
    PLUGIN_DEBUG("bridge", "Database already initialized");
    return true;
  }

  // Use sqlite3_open_v2 with SQLITE_OPEN_PRIVATECACHE to avoid shared cache
  // issues when multiple plugins open the same database file
  int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "Cannot open database: {}", sqlite3_errmsg(db_));
    return false;
  }

  PLUGIN_INFO("bridge", "Database opened successfully: {}", db_path_);

  // 启用外键约束
  if (!execute_sql("PRAGMA foreign_keys = ON;")) {
    return false;
  }

  if (!create_tables()) {
    return false;
  }

  initialized_ = true;
  return true;
}

// === SQL执行辅助函数 ===

auto DatabaseManager::execute_sql(const std::string &sql) -> bool {
  char *error_msg = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error_msg);

  if (rc != SQLITE_OK) {
    PLUGIN_ERROR("bridge", "SQL error: {}", error_msg);
    sqlite3_free(error_msg);
    return false;
  }

  return true;
}

} // namespace storage

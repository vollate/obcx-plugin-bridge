#pragma once

#include "bridge_storage_models.hpp"

#include <core/db_manager.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace bridge {

struct MediaGroupMapping {
  std::string source_platform;
  std::string media_group_id;
  std::string source_message_id;
  std::string target_platform;
  std::string target_message_id;
  std::string target_group_id;
  std::chrono::system_clock::time_point created_at;
};

class BridgeStateRepository {
public:
  BridgeStateRepository(obcx::core::DbManager &db_manager,
                        std::string db_instance,
                        std::string db_namespace = "bridge");

  void initialize_schema();

  auto add_message_mapping(const storage::MessageMapping &mapping) -> bool;
  auto get_target_message_id(const std::string &source_platform,
                             const std::string &source_message_id,
                             const std::string &target_platform)
      -> std::optional<std::string>;
  auto get_source_message_id(const std::string &target_platform,
                             const std::string &target_message_id,
                             const std::string &source_platform)
      -> std::optional<std::string>;
  auto update_message_mapping(const std::string &source_platform,
                              const std::string &source_message_id,
                              const std::string &target_platform,
                              const std::string &new_target_message_id) -> bool;
  auto delete_message_mapping(const std::string &source_platform,
                              const std::string &source_message_id,
                              const std::string &target_platform) -> bool;

  auto save_or_update_user(const storage::UserInfo &user_info,
                           bool force_update = false) -> bool;
  auto get_user(const std::string &platform, const std::string &user_id,
                const std::string &group_id = "")
      -> std::optional<storage::UserInfo>;
  auto query_user_display_name(const std::string &platform,
                               const std::string &user_id,
                               const std::string &group_id = "")
      -> std::optional<std::string>;

  auto save_sticker_cache(const storage::StickerCacheInfo &cache_info) -> bool;
  auto get_sticker_cache(const std::string &platform,
                         const std::string &sticker_hash)
      -> std::optional<storage::StickerCacheInfo>;
  auto update_sticker_last_used(const std::string &platform,
                                const std::string &sticker_hash) -> bool;
  auto update_sticker_conversion(
      const std::string &platform, const std::string &sticker_hash,
      const std::string &conversion_status,
      const std::optional<std::string> &converted_file_path = std::nullopt)
      -> bool;

  auto save_qq_sticker_mapping(const storage::QQStickerMapping &mapping)
      -> bool;
  auto get_qq_sticker_mapping(const std::string &qq_sticker_hash)
      -> std::optional<storage::QQStickerMapping>;
  auto update_qq_sticker_last_used(const std::string &qq_sticker_hash) -> bool;

  auto add_message_retry(const storage::MessageRetryInfo &retry_info) -> bool;
  auto get_pending_message_retries(
      const std::chrono::system_clock::time_point &ready_at, int limit = 100)
      -> std::vector<storage::MessageRetryInfo>;
  auto update_message_retry(
      const std::string &source_platform, const std::string &source_message_id,
      const std::string &target_platform, int retry_count,
      const std::chrono::system_clock::time_point &next_retry_at,
      const std::string &failure_reason) -> bool;
  auto remove_message_retry(const std::string &source_platform,
                            const std::string &source_message_id,
                            const std::string &target_platform) -> bool;

  auto add_media_group_mapping(const MediaGroupMapping &mapping) -> bool;
  auto get_media_group_mappings(const std::string &source_platform,
                                const std::string &media_group_id,
                                const std::string &target_platform)
      -> std::vector<MediaGroupMapping>;

  auto update_platform_heartbeat(
      const std::string &platform,
      const std::chrono::system_clock::time_point &heartbeat_time) -> bool;
  auto get_platform_heartbeat(const std::string &platform)
      -> std::optional<storage::PlatformHeartbeatInfo>;

private:
  obcx::core::DbManager &db_manager_;
  std::string db_instance_;
  std::string db_namespace_;
};

} // namespace bridge

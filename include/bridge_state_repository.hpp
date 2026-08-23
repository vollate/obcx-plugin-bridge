#pragma once

#include "bridge_message_identity.hpp"
#include "bridge_state_migration.hpp"
#include "bridge_storage_models.hpp"

#include <core/db_manager.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bridge {

enum class MessageMappingReadPurpose {
  General,
  PreSendDeduplication,
  PostSendRecovery,
};

enum class MessageMappingWritePurpose {
  General,
  DirectForward,
  RetryCompletion,
  DeferredMediaGroup,
};

enum class MappingResolutionStatus {
  Missing,
  Unique,
  Ambiguous,
  Corrupt,
};

struct MessageMappingResolution {
  MappingResolutionStatus status = MappingResolutionStatus::Missing;
  std::optional<storage::MessageMapping> mapping;
  std::string diagnostic;

  [[nodiscard]] auto unique() const noexcept -> bool {
    return status == MappingResolutionStatus::Unique && mapping.has_value();
  }
  [[nodiscard]] auto missing() const noexcept -> bool {
    return status == MappingResolutionStatus::Missing;
  }
};

struct MessageMappingOperationCounts {
  std::uint64_t general_reads = 0;
  std::uint64_t pre_send_deduplication_reads = 0;
  std::uint64_t post_send_recovery_reads = 0;
  std::uint64_t general_writes = 0;
  std::uint64_t direct_forward_writes = 0;
  std::uint64_t retry_completion_writes = 0;
  std::uint64_t deferred_media_group_writes = 0;
};

struct MediaGroupMapping {
  std::string source_installation;
  std::string source_platform;
  std::string source_conversation_id;
  std::string media_group_id;
  std::string source_message_id;
  std::string target_installation;
  std::string target_platform;
  std::string target_conversation_id;
  std::string target_message_id;
  std::string target_group_id;
  bool is_primary = false;
  std::chrono::system_clock::time_point created_at;
};

class BridgeStateRepository {
public:
  static constexpr std::int64_t current_schema_version = 3;

  BridgeStateRepository(obcx::core::DbManager &db_manager,
                        std::string db_instance,
                        std::string db_namespace = "bridge");

  void initialize_schema(
      std::optional<BridgeStateMigrationContext> migration = std::nullopt);
  void validate_schema() const;
  [[nodiscard]] auto schema_version() const -> std::int64_t;

  auto add_message_mapping(const storage::MessageMapping &mapping,
                           MessageMappingWritePurpose purpose =
                               MessageMappingWritePurpose::General) -> bool;
  auto resolve_target_mapping(
      const BridgeMessageIdentity &source, const BridgeMessageScope &target,
      MessageMappingReadPurpose purpose = MessageMappingReadPurpose::General)
      -> MessageMappingResolution;
  auto resolve_source_mapping(const BridgeMessageIdentity &target,
                              const BridgeMessageScope &source)
      -> MessageMappingResolution;
  [[nodiscard]] auto message_mapping_operation_counts() const noexcept
      -> MessageMappingOperationCounts;
  void reset_message_mapping_operation_counts() noexcept;
  auto update_message_mapping(const BridgeMessageIdentity &source,
                              const BridgeMessageScope &target,
                              const std::string &new_target_message_id) -> bool;
  auto delete_message_mapping(const BridgeMessageIdentity &source,
                              const BridgeMessageScope &target) -> bool;

  auto save_or_update_user(const storage::UserInfo &user_info,
                           bool force_update = false) -> bool;
  auto get_user(const std::string &installation_id, const std::string &platform,
                const std::string &user_id, const std::string &group_id = "")
      -> std::optional<storage::UserInfo>;
  auto query_user_display_name(const std::string &installation_id,
                               const std::string &platform,
                               const std::string &user_id,
                               const std::string &group_id = "")
      -> std::optional<std::string>;

  auto save_sticker_cache(const storage::StickerCacheInfo &cache_info) -> bool;
  auto get_sticker_cache(const std::string &installation_id,
                         const std::string &platform,
                         const std::string &sticker_hash)
      -> std::optional<storage::StickerCacheInfo>;
  auto update_sticker_last_used(const std::string &installation_id,
                                const std::string &platform,
                                const std::string &sticker_hash) -> bool;
  auto update_sticker_conversion(
      const std::string &installation_id, const std::string &platform,
      const std::string &sticker_hash, const std::string &conversion_status,
      const std::optional<std::string> &converted_file_path = std::nullopt)
      -> bool;

  auto save_qq_sticker_mapping(const storage::QQStickerMapping &mapping)
      -> bool;
  auto get_qq_sticker_mapping(const std::string &source_installation,
                              const std::string &target_installation,
                              const std::string &qq_sticker_hash)
      -> std::optional<storage::QQStickerMapping>;
  auto update_qq_sticker_last_used(const std::string &source_installation,
                                   const std::string &target_installation,
                                   const std::string &qq_sticker_hash) -> bool;

  auto add_message_retry(const storage::MessageRetryInfo &retry_info) -> bool;
  auto get_pending_message_retries(
      const std::chrono::system_clock::time_point &ready_at, int limit = 100)
      -> std::vector<storage::MessageRetryInfo>;
  auto update_message_retry(
      const BridgeMessageIdentity &source, const BridgeMessageScope &target,
      int retry_count,
      const std::chrono::system_clock::time_point &next_retry_at,
      const std::string &failure_reason) -> bool;
  auto remove_message_retry(const BridgeMessageIdentity &source,
                            const BridgeMessageScope &target) -> bool;

  auto add_media_group_mapping(const MediaGroupMapping &mapping) -> bool;
  auto get_media_group_mappings(const BridgeMessageScope &source,
                                const std::string &media_group_id,
                                const BridgeMessageScope &target)
      -> std::vector<MediaGroupMapping>;

  auto update_platform_heartbeat(
      const std::string &installation_id, const std::string &platform,
      const std::chrono::system_clock::time_point &heartbeat_time) -> bool;
  auto get_platform_heartbeat(const std::string &installation_id)
      -> std::optional<storage::PlatformHeartbeatInfo>;

private:
  obcx::core::DbManager &db_manager_;
  std::string db_instance_;
  std::string db_namespace_;
  std::atomic_uint64_t general_mapping_reads_ = 0;
  std::atomic_uint64_t pre_send_deduplication_reads_ = 0;
  std::atomic_uint64_t post_send_recovery_reads_ = 0;
  std::atomic_uint64_t general_mapping_writes_ = 0;
  std::atomic_uint64_t direct_forward_writes_ = 0;
  std::atomic_uint64_t retry_completion_writes_ = 0;
  std::atomic_uint64_t deferred_media_group_writes_ = 0;
};

} // namespace bridge

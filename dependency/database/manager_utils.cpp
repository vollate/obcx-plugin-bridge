#include "database/manager.hpp"

#include <common/logger.hpp>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>

namespace storage {

// === 时间转换辅助函数 ===

auto DatabaseManager::time_point_to_timestamp(
    const std::chrono::system_clock::time_point &tp) -> int64_t {
  return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch())
      .count();
}

auto DatabaseManager::timestamp_to_time_point(int64_t timestamp)
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point(std::chrono::seconds(timestamp));
}

// === Hash计算函数 ===

auto DatabaseManager::calculate_hash(const std::string &input) -> std::string {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context) {
    PLUGIN_ERROR("bridge", "无法创建EVP_MD_CTX");
    return "";
  }

  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    PLUGIN_ERROR("bridge", "EVP_DigestInit_ex失败");
    EVP_MD_CTX_free(context);
    return "";
  }

  if (EVP_DigestUpdate(context, input.c_str(), input.length()) != 1) {
    PLUGIN_ERROR("bridge", "EVP_DigestUpdate失败");
    EVP_MD_CTX_free(context);
    return "";
  }

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len;
  if (EVP_DigestFinal_ex(context, hash, &hash_len) != 1) {
    PLUGIN_ERROR("bridge", "EVP_DigestFinal_ex失败");
    EVP_MD_CTX_free(context);
    return "";
  }

  EVP_MD_CTX_free(context);

  std::ostringstream ss;
  for (unsigned int i = 0; i < hash_len; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(hash[i]);
  }
  return ss.str();
}

} // namespace storage

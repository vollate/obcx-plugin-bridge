#include "bridge_hash.hpp"

#include <common/logger.hpp>
#include <openssl/evp.h>

#include <iomanip>
#include <sstream>

namespace bridge {

auto calculate_hash(const std::string &input) -> std::string {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context) {
    OBCX_ERROR("Failed to create EVP_MD_CTX");
    return "";
  }

  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    OBCX_ERROR("EVP_DigestInit_ex failed");
    EVP_MD_CTX_free(context);
    return "";
  }

  if (EVP_DigestUpdate(context, input.c_str(), input.length()) != 1) {
    OBCX_ERROR("EVP_DigestUpdate failed");
    EVP_MD_CTX_free(context);
    return "";
  }

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(context, hash, &hash_len) != 1) {
    OBCX_ERROR("EVP_DigestFinal_ex failed");
    EVP_MD_CTX_free(context);
    return "";
  }

  EVP_MD_CTX_free(context);

  std::ostringstream output;
  for (unsigned int index = 0; index < hash_len; ++index) {
    output << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[index]);
  }
  return output.str();
}

} // namespace bridge

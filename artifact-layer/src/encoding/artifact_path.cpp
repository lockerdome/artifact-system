#include "encoding/artifact_path.h"

#include <array>

#include <openssl/sha.h>

#include "absl/strings/str_cat.h"
#include "encoding/base64url.h"

namespace artifact_system::encoding {

namespace {

std::array<uint8_t, sizeof(uint64_t)> Uint64BigEndianBytes(uint64_t value) {
  return {
      static_cast<uint8_t>((value >> 56U) & 0xFFU), static_cast<uint8_t>((value >> 48U) & 0xFFU), static_cast<uint8_t>((value >> 40U) & 0xFFU),
      static_cast<uint8_t>((value >> 32U) & 0xFFU), static_cast<uint8_t>((value >> 24U) & 0xFFU), static_cast<uint8_t>((value >> 16U) & 0xFFU),
      static_cast<uint8_t>((value >> 8U) & 0xFFU),  static_cast<uint8_t>(value & 0xFFU),
  };
}

std::array<uint8_t, SHA256_DIGEST_LENGTH> Sha256(std::span<const uint8_t> bytes) {
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(bytes.data(), bytes.size(), digest.data());
  return digest;
}

} // namespace

std::string ArtifactPath(uint64_t artifact_id) {
  const auto encoded_id = base64url::Encode(Uint64BigEndianBytes(artifact_id));
  return absl::StrCat("artifacts/", encoded_id);
}

std::string IndexPath(uint64_t index_definition_id, std::span<const uint8_t> encoded_key) {
  const auto encoded_prefix = base64url::Encode(Uint64BigEndianBytes(index_definition_id));
  const auto digest = Sha256(encoded_key);
  const auto encoded_hash = base64url::Encode(digest);
  return absl::StrCat("indexes/", encoded_prefix, "/", encoded_hash);
}

} // namespace artifact_system::encoding

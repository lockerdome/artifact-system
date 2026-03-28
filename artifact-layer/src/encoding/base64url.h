#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace artifact_system::encoding::base64url {

// Encodes bytes into URL-safe base64 without padding.
// Alphabet: A-Z a-z 0-9 - _
std::string Encode(std::span<const uint8_t> bytes);

// Decodes URL-safe base64 (without padding) into raw bytes.
absl::StatusOr<std::vector<uint8_t>> Decode(std::string_view encoded);

} // namespace artifact_system::encoding::base64url

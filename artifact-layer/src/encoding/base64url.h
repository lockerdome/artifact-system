#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace artifact_system::encoding::base64url {

// Encodes bytes into URL-safe base64 without padding.
// Alphabet: A-Z a-z 0-9 - _
std::string Encode(std::span<const uint8_t> bytes);

} // namespace artifact_system::encoding::base64url

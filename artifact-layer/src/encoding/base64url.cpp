#include "encoding/base64url.h"

#include <array>

namespace artifact_system::encoding::base64url {

std::string Encode(std::span<const uint8_t> bytes) {
  static constexpr std::array<char, 64> kAlphabet = {
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
      'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
      's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_',
  };

  std::string output;
  output.reserve(((bytes.size() + 2U) / 3U) * 4U);

  size_t i = 0;
  while (i + 3 <= bytes.size()) {
    const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16U) | (static_cast<uint32_t>(bytes[i + 1]) << 8U) | static_cast<uint32_t>(bytes[i + 2]);
    output.push_back(kAlphabet[(chunk >> 18U) & 0x3FU]);
    output.push_back(kAlphabet[(chunk >> 12U) & 0x3FU]);
    output.push_back(kAlphabet[(chunk >> 6U) & 0x3FU]);
    output.push_back(kAlphabet[chunk & 0x3FU]);
    i += 3;
  }

  const size_t remaining = bytes.size() - i;
  if (remaining == 1) {
    const uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16U;
    output.push_back(kAlphabet[(chunk >> 18U) & 0x3FU]);
    output.push_back(kAlphabet[(chunk >> 12U) & 0x3FU]);
  } else if (remaining == 2) {
    const uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16U) | (static_cast<uint32_t>(bytes[i + 1]) << 8U);
    output.push_back(kAlphabet[(chunk >> 18U) & 0x3FU]);
    output.push_back(kAlphabet[(chunk >> 12U) & 0x3FU]);
    output.push_back(kAlphabet[(chunk >> 6U) & 0x3FU]);
  }

  return output;
}

} // namespace artifact_system::encoding::base64url

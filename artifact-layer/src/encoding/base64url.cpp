#include "encoding/base64url.h"

#include <array>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace artifact_system::encoding::base64url {

namespace {

constexpr std::array<char, 64> kAlphabet = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
    'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_',
};

int DecodeBase64UrlChar(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '-') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return -1;
}

} // namespace

std::string Encode(std::span<const uint8_t> bytes) {
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

absl::StatusOr<std::vector<uint8_t>> Decode(std::string_view encoded) {
  if ((encoded.size() % 4U) == 1U) {
    return absl::InvalidArgumentError("invalid base64url length");
  }

  std::vector<uint8_t> output;
  output.reserve((encoded.size() * 3U) / 4U);

  size_t i = 0;
  while (i + 4U <= encoded.size()) {
    const int a = DecodeBase64UrlChar(encoded[i]);
    const int b = DecodeBase64UrlChar(encoded[i + 1]);
    const int c = DecodeBase64UrlChar(encoded[i + 2]);
    const int d = DecodeBase64UrlChar(encoded[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) {
      return absl::InvalidArgumentError(absl::StrCat("invalid base64url character at position ", i));
    }

    const uint32_t chunk = (static_cast<uint32_t>(a) << 18U) | (static_cast<uint32_t>(b) << 12U) | (static_cast<uint32_t>(c) << 6U) | static_cast<uint32_t>(d);
    output.push_back(static_cast<uint8_t>((chunk >> 16U) & 0xFFU));
    output.push_back(static_cast<uint8_t>((chunk >> 8U) & 0xFFU));
    output.push_back(static_cast<uint8_t>(chunk & 0xFFU));
    i += 4U;
  }

  const size_t remaining = encoded.size() - i;
  if (remaining == 2U) {
    const int a = DecodeBase64UrlChar(encoded[i]);
    const int b = DecodeBase64UrlChar(encoded[i + 1]);
    if (a < 0 || b < 0) {
      return absl::InvalidArgumentError(absl::StrCat("invalid base64url character at position ", i));
    }
    const uint32_t chunk = (static_cast<uint32_t>(a) << 18U) | (static_cast<uint32_t>(b) << 12U);
    output.push_back(static_cast<uint8_t>((chunk >> 16U) & 0xFFU));
  } else if (remaining == 3U) {
    const int a = DecodeBase64UrlChar(encoded[i]);
    const int b = DecodeBase64UrlChar(encoded[i + 1]);
    const int c = DecodeBase64UrlChar(encoded[i + 2]);
    if (a < 0 || b < 0 || c < 0) {
      return absl::InvalidArgumentError(absl::StrCat("invalid base64url character at position ", i));
    }
    const uint32_t chunk = (static_cast<uint32_t>(a) << 18U) | (static_cast<uint32_t>(b) << 12U) | (static_cast<uint32_t>(c) << 6U);
    output.push_back(static_cast<uint8_t>((chunk >> 16U) & 0xFFU));
    output.push_back(static_cast<uint8_t>((chunk >> 8U) & 0xFFU));
  }

  return output;
}

} // namespace artifact_system::encoding::base64url

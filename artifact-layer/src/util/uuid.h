#pragma once

#include <random>
#include <string>

#include "absl/strings/str_format.h"

namespace artifact_system::util {

/// Generate a random UUID-like 128-bit hex string (32 characters).
inline std::string GenerateUUID() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  const uint64_t hi = rng();
  const uint64_t lo = rng();
  return absl::StrFormat("%016x%016x", hi, lo);
}

} // namespace artifact_system::util

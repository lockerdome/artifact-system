#pragma once

#include <cstdint>

namespace id_allocator {

/// Compose a full ID from a bucket index and a sequential counter value.
///
/// Layout (for the default 40-bit bucket_size_bits):
///   bits 63-53  : reserved (zero)
///   bits 52-40  : bucket index
///   bits 39-0   : sequential counter
///
/// In general:
///   id = (bucket_index << bucket_size_bits) + counter
///
/// The caller is responsible for ensuring that:
///   - counter < (1 << bucket_size_bits)
///   - bucket_index fits in the remaining bits
///   - The resulting ID is within [0, 2^53) for JS safe integer compatibility.
[[nodiscard]] constexpr uint64_t compose_id(
    uint32_t bucket_index,
    uint32_t bucket_size_bits,
    uint64_t counter) {
    return (static_cast<uint64_t>(bucket_index) << bucket_size_bits) + counter;
}

/// Extract the bucket index from an ID.
[[nodiscard]] constexpr uint32_t extract_bucket_index(
    uint64_t id,
    uint32_t bucket_size_bits) {
    return static_cast<uint32_t>(id >> bucket_size_bits);
}

/// Extract the sequential counter from an ID.
[[nodiscard]] constexpr uint64_t extract_counter(
    uint64_t id,
    uint32_t bucket_size_bits) {
    return id & ((uint64_t{1} << bucket_size_bits) - 1);
}

}  // namespace id_allocator

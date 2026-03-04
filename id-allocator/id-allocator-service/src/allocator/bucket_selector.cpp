#include "allocator/bucket_selector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <random>
#include <string>

namespace id_allocator {

BucketSelector::BucketSelector(BlockStore& store, std::string partition_id, uint32_t num_buckets)
    : store_{store}, partition_id_{std::move(partition_id)}, num_buckets_{num_buckets} {
}

std::expected<uint32_t, StoreError> BucketSelector::select() {
  // Trivial case: only one bucket.
  if (num_buckets_ == 1) {
    return 0;
  }

  // Pick 2 distinct random bucket indices.
  std::uniform_int_distribution<uint32_t> dist{0, num_buckets_ - 1};

  std::array<uint32_t, kNumBucketsToCheck> candidates{};
  candidates[0] = dist(rng_);
  do {
    candidates[1] = dist(rng_);
  } while (candidates[1] == candidates[0]);

  // Query their current counts.
  auto counts_result = store_.get_bucket_counts(partition_id_, std::span<const uint32_t>{candidates.data(), candidates.size()});

  if (!counts_result) {
    return std::unexpected(counts_result.error());
  }

  auto& counts = *counts_result;

  // Return the bucket with the lowest count. Missing entries count as 0.
  auto get_count = [&](uint32_t idx) -> uint64_t {
    auto it = counts.find(idx);
    return (it != counts.end()) ? it->second : 0;
  };

  uint64_t count_a = get_count(candidates[0]);
  uint64_t count_b = get_count(candidates[1]);

  // Ties: pick the first candidate (arbitrary but deterministic per call).
  return (count_a <= count_b) ? candidates[0] : candidates[1];
}

} // namespace id_allocator

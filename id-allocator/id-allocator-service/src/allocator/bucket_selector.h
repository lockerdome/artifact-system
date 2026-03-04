#pragma once

#include <cstdint>
#include <expected>
#include <random>
#include <string>

#include "store/block_store.h"

namespace id_allocator {

class BucketSelector {
public:
  BucketSelector(BlockStore& store, std::string partition_id, uint32_t num_buckets);

  /// Select the least-loaded bucket using "power of two random choices".
  /// Picks 2 random distinct candidates, queries their current counts,
  /// and returns the one with the lower count.
  [[nodiscard]] std::expected<uint32_t, StoreError> select();

private:
  static constexpr uint32_t kNumBucketsToCheck = 2;

  BlockStore& store_;
  std::string partition_id_;
  uint32_t num_buckets_;
  std::mt19937 rng_{std::random_device{}()};
};

} // namespace id_allocator

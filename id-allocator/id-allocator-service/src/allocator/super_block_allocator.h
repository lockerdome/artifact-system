#pragma once

#include <cstdint>
#include <expected>
#include <future>
#include <mutex>
#include <utility>

#include "allocator/bucket_selector.h"
#include "allocator/super_block.h"
#include "partition/partition.h"
#include "store/block_store.h"

namespace id_allocator {

/// Double-buffered super block allocator for a single partition.
///
/// Hands out fixed-size blocks of IDs to callers (typically gRPC handler
/// threads).  Internally maintains an active and a background SuperBlock,
/// prefetching the next super block when the active one is half-depleted.
class SuperBlockAllocator {
public:
  SuperBlockAllocator(BlockStore& store, PartitionConfig config);

  /// Allocate a block of IDs.  Returns [range_start, range_end).
  /// Thread-safe — called concurrently from gRPC handler threads.
  [[nodiscard]] std::expected<std::pair<uint64_t, uint64_t>, StoreError> allocate_block();

private:
  /// Fetch a new super block from the store.
  /// Picks a bucket via the selector, reserves super_block_size IDs,
  /// and composes the resulting ID range.
  /// NOTE: Must be called while mutex_ is held (touches bucket_selector_).
  [[nodiscard]] std::expected<SuperBlock, StoreError> fetch_super_block();

  /// Fetch a super block from a specific bucket.
  /// Safe to call without holding mutex_ (only touches thread-safe store_).
  [[nodiscard]] std::expected<SuperBlock, StoreError> fetch_super_block_from(uint32_t bucket_index);

  /// Start an async prefetch if the active super block is below threshold
  /// and no fetch is already in progress.
  void maybe_start_prefetch();

  /// If a background fetch future is ready, harvest its result.
  void try_complete_prefetch();

  BlockStore& store_;
  PartitionConfig config_;
  BucketSelector bucket_selector_;

  std::mutex mutex_;
  SuperBlock active_;
  SuperBlock background_;
  std::future<std::expected<SuperBlock, StoreError>> pending_fetch_;
  bool fetch_in_progress_ = false;
};

} // namespace id_allocator

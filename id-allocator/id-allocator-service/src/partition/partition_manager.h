#pragma once

#include <expected>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "allocator/super_block_allocator.h"
#include "partition/partition.h"
#include "store/block_store.h"

namespace id_allocator {

/// Owns all partitions and their SuperBlockAllocators.
/// Main coordination point between gRPC services and storage/allocation.
class PartitionManager {
public:
    explicit PartitionManager(BlockStore& store);

    /// Load existing partitions from the store and create allocators for them.
    /// Called once at startup before the gRPC server starts accepting requests.
    std::expected<void, StoreError> initialize();

    /// Create a new partition. Validates config, saves to store, creates allocator.
    std::expected<void, StoreError> create_partition(PartitionConfig config);

    /// Get partition configuration. Returns not_found if it doesn't exist.
    std::expected<PartitionConfig, StoreError> get_partition(
        std::string_view partition_id);

    /// Delete a partition. Removes from store and tears down allocator.
    /// Idempotent — returns OK even if partition doesn't exist.
    std::expected<void, StoreError> delete_partition(
        std::string_view partition_id);

    /// Allocate a block of IDs from a partition.
    /// Returns [range_start, range_end) — exactly block_size IDs.
    /// Returns not_found if partition doesn't exist.
    std::expected<std::pair<uint64_t, uint64_t>, StoreError> allocate_block(
        std::string_view partition_id);

private:
    BlockStore& store_;

    struct PartitionEntry {
        PartitionConfig config;
        std::unique_ptr<SuperBlockAllocator> allocator;
    };

    std::shared_mutex mutex_;
    std::unordered_map<std::string, PartitionEntry> partitions_;
};

}  // namespace id_allocator

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "partition/partition.h"

namespace id_allocator {

enum class StoreError {
    not_found,
    already_exists,
    resource_exhausted,  // bucket rollover
    unavailable,         // transient store failure
    invalid_argument,
};

class BlockStore {
public:
    virtual ~BlockStore() = default;

    // Atomically increment the counter for a bucket in a partition by `increment`.
    // Returns the counter value BEFORE the increment (so the caller owns
    // [previous, previous + increment)).
    // Fails with resource_exhausted if previous_count + increment > max_value.
    virtual std::expected<uint64_t, StoreError> allocate(
        std::string_view partition_id,
        uint32_t bucket_index,
        uint64_t increment,
        uint64_t max_value) = 0;

    // Read current counters for a set of buckets in a partition.
    // Returns a map of bucket_index -> current_count.
    // Missing buckets should be returned with count 0.
    virtual std::expected<std::unordered_map<uint32_t, uint64_t>, StoreError>
    get_bucket_counts(
        std::string_view partition_id,
        std::span<const uint32_t> bucket_indices) = 0;

    // Partition CRUD
    virtual std::expected<void, StoreError> save_partition(
        const PartitionConfig& config) = 0;
    virtual std::expected<std::optional<PartitionConfig>, StoreError>
    get_partition(std::string_view partition_id) = 0;
    virtual std::expected<void, StoreError> delete_partition(
        std::string_view partition_id) = 0;
    virtual std::expected<std::vector<PartitionConfig>, StoreError>
    list_partitions() = 0;
};

}  // namespace id_allocator

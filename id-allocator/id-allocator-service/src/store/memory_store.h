#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "store/block_store.h"

namespace id_allocator {

class MemoryStore final : public BlockStore {
public:
    MemoryStore() = default;

    std::expected<uint64_t, StoreError> allocate(
        std::string_view partition_id,
        uint32_t bucket_index,
        uint64_t increment,
        uint64_t max_value) override;

    std::expected<std::unordered_map<uint32_t, uint64_t>, StoreError>
    get_bucket_counts(
        std::string_view partition_id,
        std::span<const uint32_t> bucket_indices) override;

    std::expected<void, StoreError> save_partition(
        const PartitionConfig& config) override;
    std::expected<std::optional<PartitionConfig>, StoreError>
    get_partition(std::string_view partition_id) override;
    std::expected<void, StoreError> delete_partition(
        std::string_view partition_id) override;
    std::expected<std::vector<PartitionConfig>, StoreError>
    list_partitions() override;

private:
    struct PartitionData {
        PartitionConfig config;
        std::mutex counter_mutex;
        std::unordered_map<uint32_t, uint64_t> bucket_counters;
    };

    std::shared_mutex partitions_mutex_;
    std::unordered_map<std::string, std::unique_ptr<PartitionData>> partitions_;
};

}  // namespace id_allocator

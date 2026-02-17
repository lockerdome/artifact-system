#include "partition/partition_manager.h"

#include <shared_mutex>

namespace id_allocator {

PartitionManager::PartitionManager(BlockStore& store)
    : store_(store) {}

std::expected<void, StoreError> PartitionManager::initialize() {
    auto result = store_.list_partitions();
    if (!result) {
        return std::unexpected(result.error());
    }

    std::unique_lock lock(mutex_);
    for (auto& config : *result) {
        auto allocator = std::make_unique<SuperBlockAllocator>(store_, config);
        std::string id = config.partition_id;
        partitions_.emplace(
            std::move(id),
            PartitionEntry{std::move(config), std::move(allocator)});
    }

    return {};
}

std::expected<void, StoreError> PartitionManager::create_partition(
    PartitionConfig config) {

    // Validate before acquiring the lock.
    auto validation = config.validate();
    if (!validation) {
        return std::unexpected(StoreError::invalid_argument);
    }

    std::unique_lock lock(mutex_);

    if (partitions_.contains(config.partition_id)) {
        return std::unexpected(StoreError::already_exists);
    }

    auto save_result = store_.save_partition(config);
    if (!save_result) {
        return std::unexpected(save_result.error());
    }

    auto allocator = std::make_unique<SuperBlockAllocator>(store_, config);
    std::string id = config.partition_id;
    partitions_.emplace(
        std::move(id),
        PartitionEntry{std::move(config), std::move(allocator)});

    return {};
}

std::expected<PartitionConfig, StoreError> PartitionManager::get_partition(
    std::string_view partition_id) {

    std::shared_lock lock(mutex_);

    auto it = partitions_.find(std::string(partition_id));
    if (it == partitions_.end()) {
        return std::unexpected(StoreError::not_found);
    }

    return it->second.config;
}

std::expected<void, StoreError> PartitionManager::delete_partition(
    std::string_view partition_id) {

    std::unique_lock lock(mutex_);

    auto delete_result = store_.delete_partition(partition_id);
    if (!delete_result) {
        return std::unexpected(delete_result.error());
    }

    partitions_.erase(std::string(partition_id));

    return {};
}

std::expected<std::pair<uint64_t, uint64_t>, StoreError>
PartitionManager::allocate_block(std::string_view partition_id) {

    std::shared_lock lock(mutex_);

    auto it = partitions_.find(std::string(partition_id));
    if (it == partitions_.end()) {
        return std::unexpected(StoreError::not_found);
    }

    return it->second.allocator->allocate_block();
}

}  // namespace id_allocator

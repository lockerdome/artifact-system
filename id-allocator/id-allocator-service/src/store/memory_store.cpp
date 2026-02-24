#include "store/memory_store.h"

#include <shared_mutex>

namespace id_allocator {

std::expected<uint64_t, StoreError> MemoryStore::allocate(std::string_view partition_id, uint32_t bucket_index, uint64_t increment, uint64_t max_value) {

  // Acquire a shared lock to find the partition.
  std::shared_lock read_lock(partitions_mutex_);
  auto it = partitions_.find(std::string(partition_id));
  if (it == partitions_.end()) {
    return std::unexpected(StoreError::not_found);
  }
  auto& data = *it->second;
  read_lock.unlock();

  // Lock the partition's bucket counters.
  std::lock_guard counter_lock(data.counter_mutex);
  auto& count = data.bucket_counters[bucket_index];

  if (count + increment > max_value) {
    return std::unexpected(StoreError::resource_exhausted);
  }

  uint64_t previous = count;
  count += increment;
  return previous;
}

std::expected<std::unordered_map<uint32_t, uint64_t>, StoreError> MemoryStore::get_bucket_counts(std::string_view partition_id,
                                                                                                 std::span<const uint32_t> bucket_indices) {

  std::shared_lock read_lock(partitions_mutex_);
  auto it = partitions_.find(std::string(partition_id));
  if (it == partitions_.end()) {
    return std::unexpected(StoreError::not_found);
  }
  auto& data = *it->second;
  read_lock.unlock();

  std::lock_guard counter_lock(data.counter_mutex);
  std::unordered_map<uint32_t, uint64_t> result;
  result.reserve(bucket_indices.size());

  for (uint32_t idx : bucket_indices) {
    auto cit = data.bucket_counters.find(idx);
    result[idx] = (cit != data.bucket_counters.end()) ? cit->second : 0;
  }

  return result;
}

std::expected<void, StoreError> MemoryStore::save_partition(const PartitionConfig& config) {

  std::unique_lock write_lock(partitions_mutex_);

  if (partitions_.contains(config.partition_id)) {
    return std::unexpected(StoreError::already_exists);
  }

  auto data = std::make_unique<PartitionData>();
  data->config = config;
  partitions_.emplace(config.partition_id, std::move(data));
  return {};
}

std::expected<std::optional<PartitionConfig>, StoreError> MemoryStore::get_partition(std::string_view partition_id) {

  std::shared_lock read_lock(partitions_mutex_);
  auto it = partitions_.find(std::string(partition_id));
  if (it == partitions_.end()) {
    return std::optional<PartitionConfig>{std::nullopt};
  }
  return std::optional<PartitionConfig>{it->second->config};
}

std::expected<void, StoreError> MemoryStore::delete_partition(std::string_view partition_id) {

  std::unique_lock write_lock(partitions_mutex_);
  // Idempotent: erase returns 0 if key not found, which is fine.
  partitions_.erase(std::string(partition_id));
  return {};
}

std::expected<std::vector<PartitionConfig>, StoreError> MemoryStore::list_partitions() {

  std::shared_lock read_lock(partitions_mutex_);
  std::vector<PartitionConfig> result;
  result.reserve(partitions_.size());

  for (const auto& [id, data] : partitions_) {
    result.push_back(data->config);
  }

  return result;
}

} // namespace id_allocator

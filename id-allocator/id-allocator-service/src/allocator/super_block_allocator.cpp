#include "allocator/super_block_allocator.h"

#include <chrono>
#include <expected>
#include <format>
#include <future>
#include <mutex>
#include <utility>

#include "id_format.h"

namespace id_allocator {

SuperBlockAllocator::SuperBlockAllocator(BlockStore& store, PartitionConfig config)
    : store_{store},
      config_{std::move(config)},
      bucket_selector_{store_, config_.partition_id, config_.num_buckets} {}

std::expected<std::pair<uint64_t, uint64_t>, StoreError>
SuperBlockAllocator::allocate_block() {
    std::lock_guard lock{mutex_};

    // 1. Harvest any completed background fetch.
    try_complete_prefetch();

    // 2. Try to carve from the active super block.
    if (auto block = active_.allocate_block(config_.block_size)) {
        maybe_start_prefetch();
        return *block;
    }

    // 3. Active is exhausted — promote background if available.
    if (!background_.is_empty()) {
        active_ = background_;
        background_ = SuperBlock{};

        if (auto block = active_.allocate_block(config_.block_size)) {
            maybe_start_prefetch();
            return *block;
        }
    }

    // 4. Both exhausted — wait on pending fetch if one is in flight.
    if (fetch_in_progress_ && pending_fetch_.valid()) {
        auto result = pending_fetch_.get();
        fetch_in_progress_ = false;

        if (!result) {
            return std::unexpected(result.error());
        }
        active_ = *std::move(result);

        if (auto block = active_.allocate_block(config_.block_size)) {
            maybe_start_prefetch();
            return *block;
        }
        // Newly fetched super block couldn't even serve one block —
        // this shouldn't happen with a sane config, but handle gracefully.
        return std::unexpected(StoreError::resource_exhausted);
    }

    // 5. Both exhausted, no pending fetch — synchronous fetch.
    auto result = fetch_super_block();
    if (!result) {
        return std::unexpected(result.error());
    }
    active_ = *std::move(result);

    if (auto block = active_.allocate_block(config_.block_size)) {
        maybe_start_prefetch();
        return *block;
    }

    return std::unexpected(StoreError::resource_exhausted);
}

std::expected<SuperBlock, StoreError> SuperBlockAllocator::fetch_super_block() {
    // Pick a bucket using power-of-two-random-choices.
    // NOTE: Must be called with mutex_ held (bucket_selector_ is not thread-safe).
    auto bucket_result = bucket_selector_.select();
    if (!bucket_result) {
        return std::unexpected(bucket_result.error());
    }
    return fetch_super_block_from(*bucket_result);
}

std::expected<SuperBlock, StoreError> SuperBlockAllocator::fetch_super_block_from(
    uint32_t bucket_index) {
    // Reserve super_block_size sequential counter values in that bucket.
    // This method is safe to call without the mutex — it only touches
    // thread-safe members (store_, config_ which is immutable after construction).
    auto alloc_result = store_.allocate(
        config_.partition_id,
        bucket_index,
        config_.super_block_size,
        config_.max_bucket_value());

    if (!alloc_result) {
        return std::unexpected(alloc_result.error());
    }
    uint64_t previous_count = *alloc_result;

    // Compose the full ID range from bucket index + counter values.
    uint64_t range_start = compose_id(
        bucket_index, config_.bucket_size_bits, previous_count);
    uint64_t range_end = compose_id(
        bucket_index, config_.bucket_size_bits,
        previous_count + config_.super_block_size);

    return SuperBlock{
        .range_start = range_start,
        .range_end = range_end,
        .next = range_start,
    };
}

void SuperBlockAllocator::maybe_start_prefetch() {
    if (fetch_in_progress_) {
        return;
    }

    // Start prefetch when the active block is below half capacity and
    // there's no background block ready to take over.
    bool below_threshold =
        active_.remaining() <= config_.super_block_size / 2;

    if (below_threshold && background_.is_empty()) {
        // Select the bucket while we hold the mutex (bucket_selector_ is
        // not thread-safe).  The actual store allocation is safe to run
        // concurrently since BlockStore implementations are thread-safe.
        auto bucket_result = bucket_selector_.select();
        if (!bucket_result) {
            return;  // Selection failed; will retry on next allocate_block().
        }
        uint32_t bucket_index = *bucket_result;

        fetch_in_progress_ = true;
        pending_fetch_ = std::async(
            std::launch::async,
            [this, bucket_index] {
                return fetch_super_block_from(bucket_index);
            });
    }
}

void SuperBlockAllocator::try_complete_prefetch() {
    if (!fetch_in_progress_ || !pending_fetch_.valid()) {
        return;
    }

    // Non-blocking check: is the future ready?
    auto status = pending_fetch_.wait_for(std::chrono::seconds{0});
    if (status != std::future_status::ready) {
        return;
    }

    auto result = pending_fetch_.get();
    fetch_in_progress_ = false;

    if (result) {
        background_ = *std::move(result);
    }
    // On error we silently discard — the next allocate_block() call will
    // either use remaining active_ capacity or trigger a new fetch.
}

}  // namespace id_allocator

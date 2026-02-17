#include <algorithm>
#include <cstdint>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocator/super_block_allocator.h"
#include "id_format.h"
#include "store/memory_store.h"

namespace id_allocator {
namespace {

PartitionConfig make_config(uint32_t super_block_size = 16,
                            uint32_t block_size = 4) {
    return PartitionConfig{
        .partition_id = "test-partition",
        .num_buckets = 1,
        .bucket_size_bits = 40,
        .super_block_size = super_block_size,
        .block_size = block_size,
    };
}

TEST(SuperBlockAllocatorTest, BasicAllocationReturnsNonOverlappingBlocks) {
    auto config = make_config(/*super_block_size=*/16, /*block_size=*/4);
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(config).has_value());

    SuperBlockAllocator allocator(store, config);

    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (int i = 0; i < 4; ++i) {
        auto result = allocator.allocate_block();
        ASSERT_TRUE(result.has_value()) << "block " << i;
        auto [start, end] = *result;
        EXPECT_EQ(end - start, config.block_size)
            << "block " << i << ": range_start=" << start
            << " range_end=" << end;
        ranges.push_back(*result);
    }

    // Verify no two ranges overlap.
    for (size_t i = 0; i < ranges.size(); ++i) {
        for (size_t j = i + 1; j < ranges.size(); ++j) {
            auto [s1, e1] = ranges[i];
            auto [s2, e2] = ranges[j];
            EXPECT_TRUE(e1 <= s2 || e2 <= s1)
                << "ranges overlap: [" << s1 << ", " << e1 << ") and ["
                << s2 << ", " << e2 << ")";
        }
    }
}

TEST(SuperBlockAllocatorTest, ExhaustionTriggersNewSuperBlock) {
    // super_block_size=16, block_size=4 → 4 blocks per super block.
    // Requesting 8 blocks should require 2 super blocks.
    auto config = make_config(16, 4);
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(config).has_value());

    SuperBlockAllocator allocator(store, config);

    for (int i = 0; i < 8; ++i) {
        auto result = allocator.allocate_block();
        ASSERT_TRUE(result.has_value()) << "block " << i;
        auto [start, end] = *result;
        EXPECT_EQ(end - start, config.block_size) << "block " << i;
    }
}

TEST(SuperBlockAllocatorTest, ReturnedIdsAreValidComposedIds) {
    auto config = make_config(16, 4);
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(config).has_value());

    SuperBlockAllocator allocator(store, config);

    for (int i = 0; i < 4; ++i) {
        auto result = allocator.allocate_block();
        ASSERT_TRUE(result.has_value());
        auto [start, end] = *result;

        // The bucket index should be valid for our config.
        uint32_t bucket = extract_bucket_index(start, config.bucket_size_bits);
        EXPECT_LT(bucket, config.num_buckets) << "block " << i;

        // Round-trip: recompose from extracted parts.
        uint64_t counter = extract_counter(start, config.bucket_size_bits);
        EXPECT_EQ(compose_id(bucket, config.bucket_size_bits, counter), start)
            << "block " << i;
    }
}

TEST(SuperBlockAllocatorTest, AllReturnedRangesAreNonOverlapping) {
    auto config = make_config(16, 4);
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(config).has_value());

    SuperBlockAllocator allocator(store, config);

    // Allocate many blocks (spanning multiple super blocks).
    constexpr int kTotalBlocks = 20;
    std::set<uint64_t> all_ids;

    for (int i = 0; i < kTotalBlocks; ++i) {
        auto result = allocator.allocate_block();
        ASSERT_TRUE(result.has_value()) << "block " << i;
        auto [start, end] = *result;

        for (uint64_t id = start; id < end; ++id) {
            auto [_, inserted] = all_ids.insert(id);
            EXPECT_TRUE(inserted)
                << "Duplicate ID " << id << " in block " << i;
        }
    }

    EXPECT_EQ(all_ids.size(),
              static_cast<size_t>(kTotalBlocks * config.block_size));
}

TEST(SuperBlockAllocatorTest, ConcurrentAllocationsProduceValidRanges) {
    // Use larger super blocks for concurrency test to avoid excessive
    // store round-trips.
    auto config = make_config(/*super_block_size=*/1024, /*block_size=*/4);
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(config).has_value());

    SuperBlockAllocator allocator(store, config);

    constexpr int kNumThreads = 4;
    constexpr int kBlocksPerThread = 50;

    std::vector<std::vector<std::pair<uint64_t, uint64_t>>>
        per_thread_ranges(kNumThreads);
    std::atomic<int> error_count{0};

    auto worker = [&](int thread_idx) {
        for (int i = 0; i < kBlocksPerThread; ++i) {
            auto result = allocator.allocate_block();
            if (!result.has_value()) {
                error_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                per_thread_ranges[thread_idx].push_back(*result);
            }
        }
    };

    {
        std::vector<std::jthread> threads;
        threads.reserve(kNumThreads);
        for (int i = 0; i < kNumThreads; ++i) {
            threads.emplace_back(worker, i);
        }
    }  // join all

    EXPECT_EQ(error_count.load(), 0);

    // Gather all individual IDs and verify uniqueness.
    std::set<uint64_t> all_ids;
    for (const auto& ranges : per_thread_ranges) {
        for (auto [start, end] : ranges) {
            EXPECT_EQ(end - start, config.block_size);
            for (uint64_t id = start; id < end; ++id) {
                auto [_, inserted] = all_ids.insert(id);
                EXPECT_TRUE(inserted) << "Duplicate ID: " << id;
            }
        }
    }

    EXPECT_EQ(all_ids.size(),
              static_cast<size_t>(kNumThreads * kBlocksPerThread *
                                  config.block_size));
}

}  // namespace
}  // namespace id_allocator

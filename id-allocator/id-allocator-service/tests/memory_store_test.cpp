#include <algorithm>
#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "store/memory_store.h"

namespace id_allocator {
namespace {

PartitionConfig make_config(std::string id = "test-partition") {
    return PartitionConfig{
        .partition_id = std::move(id),
        .num_buckets = 128,
        .bucket_size_bits = 40,
        .super_block_size = 65536,
        .block_size = 2048,
    };
}

// ── Partition CRUD ───────────────────────────────────────────────────────

TEST(MemoryStoreTest, SaveAndGetPartitionRoundTrip) {
    MemoryStore store;
    auto config = make_config();

    auto save_result = store.save_partition(config);
    ASSERT_TRUE(save_result.has_value());

    auto get_result = store.get_partition("test-partition");
    ASSERT_TRUE(get_result.has_value());
    ASSERT_TRUE(get_result->has_value());
    EXPECT_EQ((*get_result)->partition_id, "test-partition");
    EXPECT_EQ((*get_result)->num_buckets, 128u);
    EXPECT_EQ((*get_result)->bucket_size_bits, 40u);
    EXPECT_EQ((*get_result)->super_block_size, 65536u);
    EXPECT_EQ((*get_result)->block_size, 2048u);
}

TEST(MemoryStoreTest, SavePartitionDuplicateReturnsAlreadyExists) {
    MemoryStore store;
    auto config = make_config();

    ASSERT_TRUE(store.save_partition(config).has_value());

    auto dup_result = store.save_partition(config);
    ASSERT_FALSE(dup_result.has_value());
    EXPECT_EQ(dup_result.error(), StoreError::already_exists);
}

TEST(MemoryStoreTest, GetPartitionNonExistentReturnsNullopt) {
    MemoryStore store;
    auto result = store.get_partition("does-not-exist");
    ASSERT_TRUE(result.has_value());      // no error
    EXPECT_FALSE(result->has_value());    // nullopt
}

TEST(MemoryStoreTest, DeletePartitionExistingThenGetReturnsNullopt) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    auto del_result = store.delete_partition("test-partition");
    ASSERT_TRUE(del_result.has_value());

    auto get_result = store.get_partition("test-partition");
    ASSERT_TRUE(get_result.has_value());
    EXPECT_FALSE(get_result->has_value());
}

TEST(MemoryStoreTest, DeletePartitionNonExistentIsIdempotent) {
    MemoryStore store;
    auto result = store.delete_partition("does-not-exist");
    ASSERT_TRUE(result.has_value());
}

TEST(MemoryStoreTest, ListPartitionsReturnsAllSaved) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config("a")).has_value());
    ASSERT_TRUE(store.save_partition(make_config("b")).has_value());
    ASSERT_TRUE(store.save_partition(make_config("c")).has_value());

    auto result = store.list_partitions();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    std::vector<std::string> ids;
    for (const auto& cfg : *result) {
        ids.push_back(cfg.partition_id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"a", "b", "c"}));
}

// ── Allocate ─────────────────────────────────────────────────────────────

TEST(MemoryStoreTest, AllocateBasicIncrement) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    auto r1 = store.allocate("test-partition", /*bucket_index=*/0,
                              /*increment=*/100, /*max_value=*/1000);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 0u);  // first allocation starts at 0

    auto r2 = store.allocate("test-partition", 0, 100, 1000);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 100u);  // second allocation starts where the first left off
}

TEST(MemoryStoreTest, AllocateRolloverDetection) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    // Fill up to the max.
    auto r1 = store.allocate("test-partition", 0, 1000, 1000);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 0u);

    // Next allocation should fail: 1000 + 1 > 1000.
    auto r2 = store.allocate("test-partition", 0, 1, 1000);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), StoreError::resource_exhausted);
}

TEST(MemoryStoreTest, AllocateNonExistentPartitionReturnsNotFound) {
    MemoryStore store;
    auto result = store.allocate("nope", 0, 100, 1000);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), StoreError::not_found);
}

// ── get_bucket_counts ────────────────────────────────────────────────────

TEST(MemoryStoreTest, GetBucketCountsReturnsZeroForUnallocated) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    std::vector<uint32_t> indices{0, 1, 2};
    auto result = store.get_bucket_counts("test-partition", indices);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 3u);
    for (auto idx : indices) {
        EXPECT_EQ((*result)[idx], 0u);
    }
}

TEST(MemoryStoreTest, GetBucketCountsAfterAllocation) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    ASSERT_TRUE(store.allocate("test-partition", 0, 50, 1000).has_value());
    ASSERT_TRUE(store.allocate("test-partition", 1, 200, 1000).has_value());

    std::vector<uint32_t> indices{0, 1, 2};
    auto result = store.get_bucket_counts("test-partition", indices);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], 50u);
    EXPECT_EQ((*result)[1], 200u);
    EXPECT_EQ((*result)[2], 0u);
}

TEST(MemoryStoreTest, GetBucketCountsNonExistentPartitionReturnsNotFound) {
    MemoryStore store;
    std::vector<uint32_t> indices{0};
    auto result = store.get_bucket_counts("nope", indices);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), StoreError::not_found);
}

// ── Thread safety ────────────────────────────────────────────────────────

TEST(MemoryStoreTest, ConcurrentAllocationsProduceNonOverlappingRanges) {
    MemoryStore store;
    ASSERT_TRUE(store.save_partition(make_config()).has_value());

    constexpr int kNumThreads = 8;
    constexpr int kIterationsPerThread = 1000;
    constexpr uint64_t kIncrement = 1;
    constexpr uint64_t kMaxValue =
        static_cast<uint64_t>(kNumThreads) * kIterationsPerThread;

    std::atomic<int> error_count{0};
    std::vector<std::vector<uint64_t>> per_thread_values(kNumThreads);

    auto worker = [&](int thread_idx) {
        for (int i = 0; i < kIterationsPerThread; ++i) {
            auto result = store.allocate(
                "test-partition", 0, kIncrement, kMaxValue);
            if (!result.has_value()) {
                error_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                per_thread_values[thread_idx].push_back(*result);
            }
        }
    };

    std::vector<std::jthread> threads;
    threads.reserve(kNumThreads);
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(worker, i);
    }
    threads.clear();  // join all

    EXPECT_EQ(error_count.load(), 0);

    // Collect all returned counter values and verify uniqueness.
    std::set<uint64_t> all_values;
    for (const auto& values : per_thread_values) {
        for (auto v : values) {
            auto [_, inserted] = all_values.insert(v);
            EXPECT_TRUE(inserted) << "Duplicate counter value: " << v;
        }
    }
    EXPECT_EQ(all_values.size(),
              static_cast<size_t>(kNumThreads * kIterationsPerThread));

    // Final counter should equal total increments.
    auto final_result = store.get_bucket_counts(
        "test-partition", std::vector<uint32_t>{0});
    ASSERT_TRUE(final_result.has_value());
    EXPECT_EQ((*final_result)[0], kMaxValue);
}

}  // namespace
}  // namespace id_allocator

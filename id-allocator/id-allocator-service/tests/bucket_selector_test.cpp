#include <cstdint>

#include <gtest/gtest.h>

#include "allocator/bucket_selector.h"
#include "store/memory_store.h"

namespace id_allocator {
namespace {

PartitionConfig make_config(uint32_t num_buckets) {
    return PartitionConfig{
        .partition_id = "test-partition",
        .num_buckets = num_buckets,
        .bucket_size_bits = 40,
        .super_block_size = 65536,
        .block_size = 2048,
    };
}

TEST(BucketSelectorTest, SingleBucketAlwaysReturnsZero) {
    MemoryStore store;
    auto config = make_config(1);
    ASSERT_TRUE(store.save_partition(config).has_value());

    BucketSelector selector(store, config.partition_id, config.num_buckets);

    for (int i = 0; i < 100; ++i) {
        auto result = selector.select();
        ASSERT_TRUE(result.has_value()) << "iteration " << i;
        EXPECT_EQ(*result, 0u);
    }
}

TEST(BucketSelectorTest, TwoBucketsPreferLowerCount) {
    MemoryStore store;
    auto config = make_config(2);
    ASSERT_TRUE(store.save_partition(config).has_value());

    // Heavily load bucket 0 so that bucket 1 is always the "lower" one.
    constexpr uint64_t heavy_load = 1'000'000;
    ASSERT_TRUE(
        store.allocate(config.partition_id, 0, heavy_load,
                       config.max_bucket_value()).has_value());

    BucketSelector selector(store, config.partition_id, config.num_buckets);

    // With power-of-two-random-choices on 2 buckets, both candidates are
    // always {0, 1}, so it should always pick bucket 1 (the lighter one).
    for (int i = 0; i < 50; ++i) {
        auto result = selector.select();
        ASSERT_TRUE(result.has_value()) << "iteration " << i;
        EXPECT_EQ(*result, 1u) << "iteration " << i;
    }
}

TEST(BucketSelectorTest, SelectionReturnsValidBucketIndex) {
    MemoryStore store;
    constexpr uint32_t kNumBuckets = 64;
    auto config = make_config(kNumBuckets);
    ASSERT_TRUE(store.save_partition(config).has_value());

    BucketSelector selector(store, config.partition_id, config.num_buckets);

    for (int i = 0; i < 200; ++i) {
        auto result = selector.select();
        ASSERT_TRUE(result.has_value()) << "iteration " << i;
        EXPECT_LT(*result, kNumBuckets) << "iteration " << i;
    }
}

TEST(BucketSelectorTest, StoreErrorPropagation) {
    MemoryStore store;
    // Do NOT save the partition — get_bucket_counts should return not_found.
    BucketSelector selector(store, "nonexistent", 4);

    auto result = selector.select();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), StoreError::not_found);
}

}  // namespace
}  // namespace id_allocator

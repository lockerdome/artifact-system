#include <gtest/gtest.h>

#include "partition/partition.h"

namespace id_allocator {
namespace {

// Convenience helper: returns a valid baseline config.
PartitionConfig make_valid_config() {
    return PartitionConfig{
        .partition_id = "test-partition",
        .num_buckets = 128,
        .bucket_size_bits = 40,
        .super_block_size = 65536,
        .block_size = 2048,
    };
}

TEST(PartitionConfigTest, ValidConfigSucceeds) {
    auto config = make_valid_config();
    auto result = config.validate();
    ASSERT_TRUE(result.has_value()) << result.error();
}

TEST(PartitionConfigTest, EmptyPartitionIdReturnsError) {
    auto config = make_valid_config();
    config.partition_id = "";
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("partition_id"), std::string::npos);
}

TEST(PartitionConfigTest, ZeroNumBucketsReturnsError) {
    auto config = make_valid_config();
    config.num_buckets = 0;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("num_buckets"), std::string::npos);
}

TEST(PartitionConfigTest, ZeroBucketSizeBitsReturnsError) {
    auto config = make_valid_config();
    config.bucket_size_bits = 0;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("bucket_size_bits"), std::string::npos);
}

TEST(PartitionConfigTest, BucketSizeBits53ReturnsError) {
    auto config = make_valid_config();
    config.bucket_size_bits = 53;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("bucket_size_bits"), std::string::npos);
}

TEST(PartitionConfigTest, ZeroBlockSizeReturnsError) {
    auto config = make_valid_config();
    config.block_size = 0;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("block_size"), std::string::npos);
}

TEST(PartitionConfigTest, ZeroSuperBlockSizeReturnsError) {
    auto config = make_valid_config();
    config.super_block_size = 0;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("super_block_size"), std::string::npos);
}

TEST(PartitionConfigTest, SuperBlockSizeNotDivisibleByBlockSizeReturnsError) {
    auto config = make_valid_config();
    config.super_block_size = 65537;  // not divisible by 2048
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("multiple"), std::string::npos);
}

TEST(PartitionConfigTest, SuperBlockSizeLessThanBlockSizeReturnsError) {
    auto config = make_valid_config();
    config.block_size = 4096;
    config.super_block_size = 2048;  // less than block_size → not a multiple
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("multiple"), std::string::npos);
}

TEST(PartitionConfigTest, NumBucketsTooLargeForBucketSizeBitsReturnsError) {
    auto config = make_valid_config();
    // With bucket_size_bits=52, only 53-52=1 bit remains for the bucket index,
    // so max buckets = 2^1 = 2.  Setting num_buckets > 2 must fail.
    config.bucket_size_bits = 52;
    config.num_buckets = 3;
    auto result = config.validate();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("num_buckets"), std::string::npos);
}

TEST(PartitionConfigTest, MaxBucketValue) {
    auto config = make_valid_config();
    EXPECT_EQ(config.max_bucket_value(), uint64_t{1} << 40);
}

}  // namespace
}  // namespace id_allocator

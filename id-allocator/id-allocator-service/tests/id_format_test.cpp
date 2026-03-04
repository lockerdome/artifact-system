#include <gtest/gtest.h>

#include "id_format.h"

namespace id_allocator {
namespace {

TEST(IdFormatTest, ComposeZeroBucketZeroCounter) {
  EXPECT_EQ(compose_id(0, 40, 0), 0ULL);
}

TEST(IdFormatTest, ComposeZeroBucketCounterOne) {
  EXPECT_EQ(compose_id(0, 40, 1), 1ULL);
}

TEST(IdFormatTest, ComposeBucketOneZeroCounter) {
  EXPECT_EQ(compose_id(1, 40, 0), 1ULL << 40);
}

TEST(IdFormatTest, ComposeBucket127Counter1000) {
  EXPECT_EQ(compose_id(127, 40, 1000), (127ULL << 40) + 1000);
}

TEST(IdFormatTest, RoundTripBucketIndex) {
  constexpr uint32_t bits = 40;
  struct TestCase {
    uint32_t bucket;
    uint64_t counter;
  };
  constexpr TestCase cases[] = {
      {0, 0}, {0, 1}, {1, 0}, {1, 42}, {127, 1000}, {255, 999999}, {8191, 0},
  };

  for (auto [b, c] : cases) {
    auto id = compose_id(b, bits, c);
    EXPECT_EQ(extract_bucket_index(id, bits), b) << "bucket=" << b << " counter=" << c;
  }
}

TEST(IdFormatTest, RoundTripCounter) {
  constexpr uint32_t bits = 40;
  struct TestCase {
    uint32_t bucket;
    uint64_t counter;
  };
  constexpr TestCase cases[] = {
      {0, 0}, {0, 1}, {1, 0}, {1, 42}, {127, 1000}, {255, 999999}, {8191, 0},
  };

  for (auto [b, c] : cases) {
    auto id = compose_id(b, bits, c);
    EXPECT_EQ(extract_counter(id, bits), c) << "bucket=" << b << " counter=" << c;
  }
}

TEST(IdFormatTest, BoundaryMaxBucketMaxCounter) {
  // 13-bit bucket index (max 8191) + 40-bit counter (max 2^40 - 1)
  constexpr uint32_t bits = 40;
  constexpr uint32_t max_bucket = 8191;
  constexpr uint64_t max_counter = (1ULL << 40) - 1;

  auto id = compose_id(max_bucket, bits, max_counter);

  EXPECT_EQ(extract_bucket_index(id, bits), max_bucket);
  EXPECT_EQ(extract_counter(id, bits), max_counter);

  // Verify the composed ID is within the JS safe integer range (< 2^53).
  EXPECT_LT(id, 1ULL << 53);
}

TEST(IdFormatTest, ComposeIsConstexpr) {
  // Verify compose_id is usable in constexpr context.
  constexpr auto id = compose_id(5, 40, 100);
  static_assert(id == (5ULL << 40) + 100);
  EXPECT_EQ(id, (5ULL << 40) + 100);
}

TEST(IdFormatTest, DifferentBucketSizeBits) {
  // With bucket_size_bits=32: bucket index occupies upper bits.
  constexpr uint32_t bits = 32;
  constexpr uint32_t bucket = 10;
  constexpr uint64_t counter = 0xDEADBEEF;

  auto id = compose_id(bucket, bits, counter);
  EXPECT_EQ(extract_bucket_index(id, bits), bucket);
  EXPECT_EQ(extract_counter(id, bits), counter);
}

} // namespace
} // namespace id_allocator

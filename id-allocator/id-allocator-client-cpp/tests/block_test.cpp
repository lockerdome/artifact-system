#include <stdexcept>

#include <gtest/gtest.h>

#include "id_allocator_client/block.h"

namespace id_allocator::client {
namespace {

TEST(BlockTest, FillAndAllocateSequentially) {
  Block block;
  block.fill(100, 103);

  EXPECT_EQ(block.remaining(), 3u);
  EXPECT_EQ(block.allocate_id(), 100u);
  EXPECT_EQ(block.allocate_id(), 101u);
  EXPECT_EQ(block.allocate_id(), 102u);
  EXPECT_FALSE(block.has_id());
}

TEST(BlockTest, AllocateThrowsWhenExhausted) {
  Block block;
  EXPECT_THROW((void)block.allocate_id(), std::runtime_error);
}

TEST(BlockTest, FillThrowsWhenBlockStillHasIds) {
  Block block;
  block.fill(10, 12);
  EXPECT_THROW(block.fill(20, 22), std::runtime_error);
}

} // namespace
} // namespace id_allocator::client

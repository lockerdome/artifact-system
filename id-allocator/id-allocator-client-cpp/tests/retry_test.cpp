#include <stdexcept>

#include <gtest/gtest.h>

#include "retry.h"

namespace id_allocator::client {
namespace {

TEST(RetryTest, ReturnsOnFirstSuccess) {
  int calls = 0;
  const int result = retry_with_backoff(
      [&calls] {
        ++calls;
        return 42;
      },
      RetryOptions{
          .max_retries = 3,
          .base_delay_ms = 1,
          .max_delay_ms = 2,
      });

  EXPECT_EQ(result, 42);
  EXPECT_EQ(calls, 1);
}

TEST(RetryTest, RetriesThenSucceeds) {
  int calls = 0;
  const int result = retry_with_backoff(
      [&calls] {
        ++calls;
        if (calls < 3) {
          throw std::runtime_error("transient");
        }
        return 9;
      },
      RetryOptions{
          .max_retries = 4,
          .base_delay_ms = 1,
          .max_delay_ms = 2,
      });

  EXPECT_EQ(result, 9);
  EXPECT_EQ(calls, 3);
}

TEST(RetryTest, ThrowsLastErrorWhenExhausted) {
  int calls = 0;

  EXPECT_THROW(retry_with_backoff(
                   [&calls]() -> int {
                     ++calls;
                     throw std::runtime_error("persistent");
                   },
                   RetryOptions{
                       .max_retries = 2,
                       .base_delay_ms = 1,
                       .max_delay_ms = 2,
                   }),
               std::runtime_error);
  EXPECT_EQ(calls, 3);
}

} // namespace
} // namespace id_allocator::client

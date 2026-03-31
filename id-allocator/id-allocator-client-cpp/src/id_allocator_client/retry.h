#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>

namespace id_allocator::client {

struct RetryOptions {
  uint32_t max_retries = 5;
  uint32_t base_delay_ms = 100;
  uint32_t max_delay_ms = 10000;
};

template <typename Fn> auto retry_with_backoff(Fn&& fn, const RetryOptions& options) -> decltype(fn()) {
  std::exception_ptr last_error;

  for (uint32_t attempt = 0; attempt <= options.max_retries; ++attempt) {
    try {
      return fn();
    } catch (...) {
      last_error = std::current_exception();
      if (attempt == options.max_retries) {
        break;
      }

      const auto exponential_delay = static_cast<double>(options.base_delay_ms) * std::pow(2.0, static_cast<double>(attempt));
      const auto capped_delay = std::min<double>(exponential_delay, static_cast<double>(options.max_delay_ms));

      static thread_local std::mt19937 rng{std::random_device{}()};
      std::uniform_real_distribution<double> jitter_dist(0.0, capped_delay);
      const auto jittered_delay_ms = static_cast<uint32_t>(jitter_dist(rng));

      std::this_thread::sleep_for(std::chrono::milliseconds(jittered_delay_ms));
    }
  }

  std::rethrow_exception(last_error);
}

} // namespace id_allocator::client

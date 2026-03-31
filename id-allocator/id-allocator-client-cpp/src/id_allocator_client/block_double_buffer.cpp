#include "id_allocator_client/block_double_buffer.h"

#include <iostream>
#include <stdexcept>

namespace id_allocator::client {

BlockDoubleBuffer::BlockDoubleBuffer(uint64_t high_water_mark, FetchBlockFn fetch_block)
    : high_water_mark_(high_water_mark), fetch_block_(std::move(fetch_block)) {
  if (high_water_mark_ < 1) {
    throw std::runtime_error("high_water_mark must be a positive integer");
  }
  if (!fetch_block_) {
    throw std::runtime_error("fetch_block must be a function");
  }
}

BlockDoubleBuffer::~BlockDoubleBuffer() {
  close();
}

void BlockDoubleBuffer::initialize() {
  const auto [range_start, range_end] = fetch_block_();

  std::lock_guard lock(mutex_);
  front_.fill(range_start, range_end);
}

uint64_t BlockDoubleBuffer::allocate_id() {
  std::lock_guard lock(mutex_);

  if (!front_.has_id()) {
    if (!back_.has_id()) {
      throw std::runtime_error("ID pool depleted: both front and back blocks are exhausted");
    }
    swap_blocks_locked();
  }

  const uint64_t id = front_.allocate_id();
  if (front_.remaining() <= high_water_mark_) {
    maybe_prefetch_locked();
  }

  return id;
}

void BlockDoubleBuffer::close() {
  std::optional<std::jthread> thread_to_join;
  {
    std::lock_guard lock(mutex_);
    if (prefetch_thread_.has_value()) {
      prefetch_thread_->request_stop();
      thread_to_join = std::move(prefetch_thread_);
      prefetch_thread_.reset();
    }
    fetch_in_progress_ = false;
    front_.reset();
    back_.reset();
  }

  thread_to_join.reset();
}

void BlockDoubleBuffer::maybe_prefetch_locked() {
  if (fetch_in_progress_ || back_.has_id()) {
    return;
  }

  if (prefetch_thread_.has_value() && !fetch_in_progress_) {
    prefetch_thread_.reset();
  }

  fetch_in_progress_ = true;
  prefetch_thread_.emplace([this](std::stop_token) { prefetch_worker(); });
}

void BlockDoubleBuffer::swap_blocks_locked() {
  std::swap(front_, back_);
  back_.reset();
}

void BlockDoubleBuffer::prefetch_worker() {
  try {
    const auto [range_start, range_end] = fetch_block_();
    std::lock_guard lock(mutex_);
    back_.fill(range_start, range_end);
    fetch_in_progress_ = false;
  } catch (const std::exception& err) {
    std::cerr << "Failed to prefetch ID block: " << err.what() << '\n';
    std::lock_guard lock(mutex_);
    fetch_in_progress_ = false;
  } catch (...) {
    std::cerr << "Failed to prefetch ID block: unknown error\n";
    std::lock_guard lock(mutex_);
    fetch_in_progress_ = false;
  }
}

} // namespace id_allocator::client

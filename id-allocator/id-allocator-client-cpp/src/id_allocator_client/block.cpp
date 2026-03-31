#include "id_allocator_client/block.h"

#include <stdexcept>

namespace id_allocator::client {

uint64_t Block::remaining() const {
  return range_end_ - range_start_;
}

bool Block::has_id() const {
  return range_start_ < range_end_;
}

uint64_t Block::allocate_id() {
  if (!has_id()) {
    throw std::runtime_error("Block exhausted");
  }

  return range_start_++;
}

void Block::fill(uint64_t range_start, uint64_t range_end) {
  if (has_id()) {
    throw std::runtime_error("Cannot fill a block that still has IDs");
  }

  range_start_ = range_start;
  range_end_ = range_end;
}

void Block::reset() {
  range_start_ = 0;
  range_end_ = 0;
}

} // namespace id_allocator::client

#pragma once

#include <cstdint>

namespace id_allocator::client {

struct BlockRange {
  uint64_t range_start;
  uint64_t range_end;
};

} // namespace id_allocator::client

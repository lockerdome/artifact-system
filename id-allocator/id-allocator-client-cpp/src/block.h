#pragma once

#include <cstdint>

namespace id_allocator::client {

class Block {
public:
  [[nodiscard]] uint64_t remaining() const;
  [[nodiscard]] bool has_id() const;
  uint64_t allocate_id();
  void fill(uint64_t range_start, uint64_t range_end);
  void reset();

private:
  uint64_t range_start_ = 0;
  uint64_t range_end_ = 0;
};

} // namespace id_allocator::client

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "block.h"
#include "types.h"

namespace id_allocator::client {

using FetchBlockFn = std::function<BlockRange()>;

class BlockDoubleBuffer {
public:
  BlockDoubleBuffer(uint64_t high_water_mark, FetchBlockFn fetch_block);
  ~BlockDoubleBuffer();

  void initialize();
  uint64_t allocate_id();
  void close();

private:
  void maybe_prefetch_locked();
  void swap_blocks_locked();
  void prefetch_worker();

  uint64_t high_water_mark_;
  FetchBlockFn fetch_block_;
  Block front_;
  Block back_;

  std::mutex mutex_;
  bool fetch_in_progress_ = false;
  std::optional<std::jthread> prefetch_thread_;
};

} // namespace id_allocator::client

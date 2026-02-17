#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace id_allocator {

/// A contiguous range of IDs [range_start, range_end) that is carved into
/// fixed-size blocks on demand.
struct SuperBlock {
    uint64_t range_start = 0;  // inclusive
    uint64_t range_end = 0;    // exclusive
    uint64_t next = 0;         // next ID to carve from (starts at range_start)

    /// Carve out a block of `block_size` IDs.
    /// Returns [start, end) or std::nullopt if fewer than block_size IDs remain.
    [[nodiscard]] std::optional<std::pair<uint64_t, uint64_t>>
    allocate_block(uint32_t block_size) {
        if (remaining() < block_size) {
            return std::nullopt;
        }
        uint64_t block_start = next;
        next += block_size;
        return std::pair{block_start, next};
    }

    /// Number of IDs remaining in this super block.
    [[nodiscard]] uint64_t remaining() const {
        return (next < range_end) ? (range_end - next) : 0;
    }

    /// True if no more IDs can be carved from this super block.
    [[nodiscard]] bool is_exhausted() const {
        return remaining() == 0;
    }

    /// True if this super block has never been initialized (zero-size range).
    [[nodiscard]] bool is_empty() const {
        return range_start == range_end;
    }
};

}  // namespace id_allocator

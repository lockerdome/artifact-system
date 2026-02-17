#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace id_allocator {

struct PartitionConfig {
    std::string partition_id;
    uint32_t num_buckets = 0;
    uint32_t bucket_size_bits = 0;
    uint32_t super_block_size = 0;
    uint32_t block_size = 0;

    /// Maximum sequential counter value within a single bucket.
    [[nodiscard]] uint64_t max_bucket_value() const {
        return (uint64_t{1} << bucket_size_bits);
    }

    /// Validate the partition configuration.
    /// Returns std::unexpected with a human-readable error message on failure.
    [[nodiscard]] std::expected<void, std::string> validate() const {
        if (partition_id.empty()) {
            return std::unexpected("partition_id must not be empty");
        }
        if (num_buckets == 0) {
            return std::unexpected("num_buckets must be greater than 0");
        }
        if (bucket_size_bits == 0 || bucket_size_bits > 52) {
            return std::unexpected("bucket_size_bits must be in [1, 52]");
        }
        if (block_size == 0) {
            return std::unexpected("block_size must be greater than 0");
        }
        if (super_block_size == 0) {
            return std::unexpected("super_block_size must be greater than 0");
        }
        if (super_block_size % block_size != 0) {
            return std::unexpected(
                "super_block_size must be an exact multiple of block_size");
        }
        // Note: if super_block_size > 0 && block_size > 0 && super_block_size % block_size == 0,
        // then super_block_size >= block_size is guaranteed. No separate check needed.

        // Check that num_buckets fits in the remaining bits.
        // Total bits available = 53 (JS safe int) - bucket_size_bits.
        // The bucket index field must be wide enough.
        uint32_t bucket_index_bits = 53 - bucket_size_bits;
        // We reserve the top 11 bits, leaving 53 - 11 = 42 usable bits
        // that split between bucket index and sequential. The bucket index
        // bits = 53 - 11 - bucket_size_bits.  Actually the simpler check:
        // num_buckets must be representable.
        if (bucket_index_bits == 0) {
            return std::unexpected("no bits remaining for bucket index");
        }
        uint64_t max_buckets = uint64_t{1} << bucket_index_bits;
        if (num_buckets > max_buckets) {
            return std::unexpected(
                "num_buckets exceeds the maximum representable with "
                "the given bucket_size_bits");
        }

        return {};
    }
};

}  // namespace id_allocator

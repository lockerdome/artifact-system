#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "google/datastore/v1/datastore.grpc.pb.h"

#include "store/block_store.h"

namespace id_allocator {

class DatastoreStore final : public BlockStore {
public:
    // Constructor takes a GCP project ID and a gRPC channel (injected for
    // testability — production callers pass grpc::CreateChannel(...)).
    DatastoreStore(std::string project_id,
                   std::shared_ptr<grpc::Channel> channel);

    std::expected<uint64_t, StoreError> allocate(
        std::string_view partition_id,
        uint32_t bucket_index,
        uint64_t increment,
        uint64_t max_value) override;

    std::expected<std::unordered_map<uint32_t, uint64_t>, StoreError>
    get_bucket_counts(
        std::string_view partition_id,
        std::span<const uint32_t> bucket_indices) override;

    std::expected<void, StoreError> save_partition(
        const PartitionConfig& config) override;
    std::expected<std::optional<PartitionConfig>, StoreError>
    get_partition(std::string_view partition_id) override;
    std::expected<void, StoreError> delete_partition(
        std::string_view partition_id) override;
    std::expected<std::vector<PartitionConfig>, StoreError>
    list_partitions() override;

private:
    // ── Helpers ──────────────────────────────────────────────────────────

    // Build a Datastore Key with the project's PartitionId set.
    google::datastore::v1::Key make_key(std::string_view kind,
                                        std::string_view name) const;

    // Build a bucket entity for upsert.
    google::datastore::v1::Entity make_bucket_entity(
        std::string_view partition_id,
        uint32_t bucket_index,
        uint64_t count) const;

    // Build a partition config entity for upsert.
    google::datastore::v1::Entity make_partition_entity(
        const PartitionConfig& config) const;

    // Extract a PartitionConfig from a Datastore entity.
    static PartitionConfig entity_to_partition(
        const google::datastore::v1::Entity& entity);

    // Extract the integer "count" property from an entity, defaulting to 0.
    static uint64_t entity_to_count(
        const google::datastore::v1::Entity& entity);

    // Map a gRPC status to StoreError.
    static StoreError map_grpc_error(const grpc::Status& status);

    // Kind strings.
    static std::string bucket_kind(std::string_view partition_id);
    static std::string bucket_key_name(uint32_t bucket_index);
    static constexpr const char* kPartitionKind = "IdPartition";

    // ── Data ─────────────────────────────────────────────────────────────

    std::string project_id_;
    std::unique_ptr<google::datastore::v1::Datastore::Stub> stub_;
};

}  // namespace id_allocator

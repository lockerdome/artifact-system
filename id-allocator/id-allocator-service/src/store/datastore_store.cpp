#include "store/datastore_store.h"

#include <charconv>
#include <format>
#include <string>

#include <grpcpp/grpcpp.h>
#include "google/datastore/v1/datastore.grpc.pb.h"
#include "google/datastore/v1/entity.pb.h"
#include "google/datastore/v1/query.pb.h"

namespace id_allocator {

namespace ds = google::datastore::v1;

// ─── Constructor ────────────────────────────────────────────────────────────

DatastoreStore::DatastoreStore(std::string project_id,
                               std::shared_ptr<grpc::Channel> channel)
    : project_id_(std::move(project_id)),
      stub_(ds::Datastore::NewStub(std::move(channel))) {}

// ─── Helpers ────────────────────────────────────────────────────────────────

std::string DatastoreStore::bucket_kind(std::string_view partition_id) {
    return std::format("IdBucket:{}", partition_id);
}

std::string DatastoreStore::bucket_key_name(uint32_t bucket_index) {
    return std::format("bucket-{}", bucket_index);
}

google::datastore::v1::Key DatastoreStore::make_key(
    std::string_view kind, std::string_view name) const {
    ds::Key key;
    key.mutable_partition_id()->set_project_id(project_id_);
    auto* element = key.add_path();
    element->set_kind(std::string(kind));
    element->set_name(std::string(name));
    return key;
}

google::datastore::v1::Entity DatastoreStore::make_bucket_entity(
    std::string_view partition_id,
    uint32_t bucket_index,
    uint64_t count) const {
    ds::Entity entity;
    *entity.mutable_key() =
        make_key(bucket_kind(partition_id), bucket_key_name(bucket_index));

    ds::Value count_value;
    count_value.set_integer_value(static_cast<int64_t>(count));
    (*entity.mutable_properties())["count"] = std::move(count_value);

    return entity;
}

google::datastore::v1::Entity DatastoreStore::make_partition_entity(
    const PartitionConfig& config) const {
    ds::Entity entity;
    *entity.mutable_key() = make_key(kPartitionKind, config.partition_id);

    auto& props = *entity.mutable_properties();

    ds::Value num_buckets;
    num_buckets.set_integer_value(static_cast<int64_t>(config.num_buckets));
    props["num_buckets"] = std::move(num_buckets);

    ds::Value bucket_size_bits;
    bucket_size_bits.set_integer_value(
        static_cast<int64_t>(config.bucket_size_bits));
    props["bucket_size_bits"] = std::move(bucket_size_bits);

    ds::Value super_block_size;
    super_block_size.set_integer_value(
        static_cast<int64_t>(config.super_block_size));
    props["super_block_size"] = std::move(super_block_size);

    ds::Value block_size;
    block_size.set_integer_value(static_cast<int64_t>(config.block_size));
    props["block_size"] = std::move(block_size);

    return entity;
}

PartitionConfig DatastoreStore::entity_to_partition(
    const ds::Entity& entity) {
    PartitionConfig config;

    // The partition_id is the key's name.
    if (entity.key().path_size() > 0) {
        config.partition_id = entity.key().path(
            entity.key().path_size() - 1).name();
    }

    const auto& props = entity.properties();
    if (auto it = props.find("num_buckets"); it != props.end()) {
        config.num_buckets =
            static_cast<uint32_t>(it->second.integer_value());
    }
    if (auto it = props.find("bucket_size_bits"); it != props.end()) {
        config.bucket_size_bits =
            static_cast<uint32_t>(it->second.integer_value());
    }
    if (auto it = props.find("super_block_size"); it != props.end()) {
        config.super_block_size =
            static_cast<uint32_t>(it->second.integer_value());
    }
    if (auto it = props.find("block_size"); it != props.end()) {
        config.block_size =
            static_cast<uint32_t>(it->second.integer_value());
    }

    return config;
}

uint64_t DatastoreStore::entity_to_count(const ds::Entity& entity) {
    const auto& props = entity.properties();
    if (auto it = props.find("count"); it != props.end()) {
        return static_cast<uint64_t>(it->second.integer_value());
    }
    return 0;
}

StoreError DatastoreStore::map_grpc_error(const grpc::Status& status) {
    switch (status.error_code()) {
        case grpc::StatusCode::NOT_FOUND:
            return StoreError::not_found;
        case grpc::StatusCode::ALREADY_EXISTS:
            return StoreError::already_exists;
        case grpc::StatusCode::RESOURCE_EXHAUSTED:
            return StoreError::resource_exhausted;
        case grpc::StatusCode::INVALID_ARGUMENT:
            return StoreError::invalid_argument;
        default:
            return StoreError::unavailable;
    }
}

// ─── allocate() ─────────────────────────────────────────────────────────────

std::expected<uint64_t, StoreError> DatastoreStore::allocate(
    std::string_view partition_id,
    uint32_t bucket_index,
    uint64_t increment,
    uint64_t max_value) {

    grpc::ClientContext ctx;

    // 1. BeginTransaction (read-write).
    ds::BeginTransactionRequest begin_req;
    begin_req.set_project_id(project_id_);
    begin_req.mutable_transaction_options()->mutable_read_write();

    ds::BeginTransactionResponse begin_resp;
    auto begin_status = stub_->BeginTransaction(&ctx, begin_req, &begin_resp);
    if (!begin_status.ok()) {
        return std::unexpected(map_grpc_error(begin_status));
    }
    const std::string& txn = begin_resp.transaction();

    // 2. Lookup the bucket entity within the transaction.
    ds::LookupRequest lookup_req;
    lookup_req.set_project_id(project_id_);
    lookup_req.mutable_read_options()->set_transaction(txn);
    *lookup_req.add_keys() =
        make_key(bucket_kind(partition_id), bucket_key_name(bucket_index));

    ds::LookupResponse lookup_resp;
    grpc::ClientContext lookup_ctx;
    auto lookup_status = stub_->Lookup(&lookup_ctx, lookup_req, &lookup_resp);
    if (!lookup_status.ok()) {
        // Best-effort rollback.
        ds::RollbackRequest rb;
        rb.set_project_id(project_id_);
        rb.set_transaction(txn);
        ds::RollbackResponse rb_resp;
        grpc::ClientContext rb_ctx;
        stub_->Rollback(&rb_ctx, rb, &rb_resp);
        return std::unexpected(map_grpc_error(lookup_status));
    }

    // 3. Extract current count (0 if entity not found).
    uint64_t current = 0;
    if (!lookup_resp.found().empty()) {
        current = entity_to_count(lookup_resp.found(0).entity());
    }

    // 4. Check rollover.
    if (current + increment > max_value) {
        ds::RollbackRequest rb;
        rb.set_project_id(project_id_);
        rb.set_transaction(txn);
        ds::RollbackResponse rb_resp;
        grpc::ClientContext rb_ctx;
        stub_->Rollback(&rb_ctx, rb, &rb_resp);
        return std::unexpected(StoreError::resource_exhausted);
    }

    // 5. Upsert entity with count = current + increment.
    ds::CommitRequest commit_req;
    commit_req.set_project_id(project_id_);
    commit_req.set_transaction(txn);
    commit_req.set_mode(ds::CommitRequest::TRANSACTIONAL);

    auto* mutation = commit_req.add_mutations();
    *mutation->mutable_upsert() =
        make_bucket_entity(partition_id, bucket_index, current + increment);

    ds::CommitResponse commit_resp;
    grpc::ClientContext commit_ctx;
    auto commit_status =
        stub_->Commit(&commit_ctx, commit_req, &commit_resp);
    if (!commit_status.ok()) {
        return std::unexpected(map_grpc_error(commit_status));
    }

    // 6. Return the previous count.
    return current;
}

// ─── get_bucket_counts() ────────────────────────────────────────────────────

std::expected<std::unordered_map<uint32_t, uint64_t>, StoreError>
DatastoreStore::get_bucket_counts(
    std::string_view partition_id,
    std::span<const uint32_t> bucket_indices) {

    ds::LookupRequest req;
    req.set_project_id(project_id_);
    // Strong consistency read (no transaction needed).

    std::string kind = bucket_kind(partition_id);
    for (uint32_t idx : bucket_indices) {
        *req.add_keys() = make_key(kind, bucket_key_name(idx));
    }

    ds::LookupResponse resp;
    grpc::ClientContext ctx;
    auto status = stub_->Lookup(&ctx, req, &resp);
    if (!status.ok()) {
        return std::unexpected(map_grpc_error(status));
    }

    std::unordered_map<uint32_t, uint64_t> result;
    result.reserve(bucket_indices.size());

    // Initialize all requested buckets to 0.
    for (uint32_t idx : bucket_indices) {
        result[idx] = 0;
    }

    // Overwrite with actual values from found entities.
    for (const auto& found : resp.found()) {
        const auto& entity = found.entity();
        if (entity.key().path_size() > 0) {
            const auto& key_name =
                entity.key().path(entity.key().path_size() - 1).name();
            // Parse bucket index from "bucket-{index}".
            if (key_name.starts_with("bucket-")) {
                uint32_t idx = 0;
                auto [ptr, ec] = std::from_chars(
                    key_name.data() + 7,
                    key_name.data() + key_name.size(),
                    idx);
                if (ec == std::errc{}) {
                    result[idx] = entity_to_count(entity);
                }
            }
        }
    }

    return result;
}

// ─── save_partition() ───────────────────────────────────────────────────────

std::expected<void, StoreError> DatastoreStore::save_partition(
    const PartitionConfig& config) {

    grpc::ClientContext begin_ctx;

    // Begin a read-write transaction to check-then-insert atomically.
    ds::BeginTransactionRequest begin_req;
    begin_req.set_project_id(project_id_);
    begin_req.mutable_transaction_options()->mutable_read_write();

    ds::BeginTransactionResponse begin_resp;
    auto begin_status =
        stub_->BeginTransaction(&begin_ctx, begin_req, &begin_resp);
    if (!begin_status.ok()) {
        return std::unexpected(map_grpc_error(begin_status));
    }
    const std::string& txn = begin_resp.transaction();

    // Lookup to check existence.
    ds::LookupRequest lookup_req;
    lookup_req.set_project_id(project_id_);
    lookup_req.mutable_read_options()->set_transaction(txn);
    *lookup_req.add_keys() = make_key(kPartitionKind, config.partition_id);

    ds::LookupResponse lookup_resp;
    grpc::ClientContext lookup_ctx;
    auto lookup_status =
        stub_->Lookup(&lookup_ctx, lookup_req, &lookup_resp);
    if (!lookup_status.ok()) {
        ds::RollbackRequest rb;
        rb.set_project_id(project_id_);
        rb.set_transaction(txn);
        ds::RollbackResponse rb_resp;
        grpc::ClientContext rb_ctx;
        stub_->Rollback(&rb_ctx, rb, &rb_resp);
        return std::unexpected(map_grpc_error(lookup_status));
    }

    if (!lookup_resp.found().empty()) {
        // Partition already exists — rollback and report.
        ds::RollbackRequest rb;
        rb.set_project_id(project_id_);
        rb.set_transaction(txn);
        ds::RollbackResponse rb_resp;
        grpc::ClientContext rb_ctx;
        stub_->Rollback(&rb_ctx, rb, &rb_resp);
        return std::unexpected(StoreError::already_exists);
    }

    // Insert the partition entity.
    ds::CommitRequest commit_req;
    commit_req.set_project_id(project_id_);
    commit_req.set_transaction(txn);
    commit_req.set_mode(ds::CommitRequest::TRANSACTIONAL);

    auto* mutation = commit_req.add_mutations();
    *mutation->mutable_insert() = make_partition_entity(config);

    ds::CommitResponse commit_resp;
    grpc::ClientContext commit_ctx;
    auto commit_status =
        stub_->Commit(&commit_ctx, commit_req, &commit_resp);
    if (!commit_status.ok()) {
        return std::unexpected(map_grpc_error(commit_status));
    }

    return {};
}

// ─── get_partition() ────────────────────────────────────────────────────────

std::expected<std::optional<PartitionConfig>, StoreError>
DatastoreStore::get_partition(std::string_view partition_id) {

    ds::LookupRequest req;
    req.set_project_id(project_id_);
    *req.add_keys() = make_key(kPartitionKind, partition_id);

    ds::LookupResponse resp;
    grpc::ClientContext ctx;
    auto status = stub_->Lookup(&ctx, req, &resp);
    if (!status.ok()) {
        return std::unexpected(map_grpc_error(status));
    }

    if (resp.found().empty()) {
        return std::optional<PartitionConfig>{std::nullopt};
    }

    return std::optional<PartitionConfig>{
        entity_to_partition(resp.found(0).entity())};
}

// ─── delete_partition() ─────────────────────────────────────────────────────

std::expected<void, StoreError> DatastoreStore::delete_partition(
    std::string_view partition_id) {

    ds::CommitRequest req;
    req.set_project_id(project_id_);
    req.set_mode(ds::CommitRequest::NON_TRANSACTIONAL);

    auto* mutation = req.add_mutations();
    *mutation->mutable_delete_() = make_key(kPartitionKind, partition_id);

    ds::CommitResponse resp;
    grpc::ClientContext ctx;
    auto status = stub_->Commit(&ctx, req, &resp);
    if (!status.ok()) {
        // Datastore delete of a non-existent key is not an error, but if the
        // RPC itself failed we report it.
        return std::unexpected(map_grpc_error(status));
    }

    return {};
}

// ─── list_partitions() ──────────────────────────────────────────────────────

std::expected<std::vector<PartitionConfig>, StoreError>
DatastoreStore::list_partitions() {

    std::vector<PartitionConfig> results;
    std::string cursor;  // pagination cursor; empty on first request.

    for (;;) {
        ds::RunQueryRequest req;
        req.set_project_id(project_id_);

        auto* query = req.mutable_query();
        auto* kind = query->add_kind();
        kind->set_name(kPartitionKind);

        if (!cursor.empty()) {
            query->set_start_cursor(cursor);
        }

        ds::RunQueryResponse resp;
        grpc::ClientContext ctx;
        auto status = stub_->RunQuery(&ctx, req, &resp);
        if (!status.ok()) {
            return std::unexpected(map_grpc_error(status));
        }

        const auto& batch = resp.batch();
        for (const auto& result : batch.entity_results()) {
            results.push_back(entity_to_partition(result.entity()));
        }

        // Check if there are more results.
        if (batch.more_results() ==
                ds::QueryResultBatch::NOT_FINISHED) {
            cursor = batch.end_cursor();
        } else {
            break;
        }
    }

    return results;
}

}  // namespace id_allocator

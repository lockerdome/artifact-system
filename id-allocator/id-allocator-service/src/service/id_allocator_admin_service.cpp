#include "service/id_allocator_admin_service.h"

#include <format>
#include <string>

namespace id_allocator {

IdAllocatorAdminServiceImpl::IdAllocatorAdminServiceImpl(
    PartitionManager& partition_manager)
    : partition_manager_{partition_manager} {}

grpc::Status IdAllocatorAdminServiceImpl::CreatePartition(
    grpc::ServerContext* /*context*/,
    const CreatePartitionRequest* request,
    CreatePartitionResponse* /*response*/) {

    PartitionConfig config{
        .partition_id = request->partition_id(),
        .num_buckets = request->num_buckets(),
        .bucket_size_bits = request->bucket_size_bits(),
        .super_block_size = request->super_block_size(),
        .block_size = request->block_size(),
    };

    auto validation = config.validate();
    if (!validation) {
        return {grpc::StatusCode::INVALID_ARGUMENT, validation.error()};
    }

    auto result = partition_manager_.create_partition(std::move(config));
    if (!result) {
        switch (result.error()) {
            case StoreError::already_exists:
                return {grpc::StatusCode::ALREADY_EXISTS,
                        std::format("partition already exists: {}",
                                    request->partition_id())};
            case StoreError::invalid_argument:
                return {grpc::StatusCode::INVALID_ARGUMENT,
                        std::format("invalid partition configuration: {}",
                                    request->partition_id())};
            case StoreError::unavailable:
                return {grpc::StatusCode::UNAVAILABLE,
                        "backing store unavailable"};
            default:
                return {grpc::StatusCode::INTERNAL,
                        std::format("internal error creating partition: {}",
                                    request->partition_id())};
        }
    }

    return grpc::Status::OK;
}

grpc::Status IdAllocatorAdminServiceImpl::GetPartition(
    grpc::ServerContext* /*context*/,
    const GetPartitionRequest* request,
    GetPartitionResponse* response) {

    auto result = partition_manager_.get_partition(request->partition_id());
    if (!result) {
        switch (result.error()) {
            case StoreError::not_found:
                return {grpc::StatusCode::NOT_FOUND,
                        std::format("partition not found: {}",
                                    request->partition_id())};
            case StoreError::unavailable:
                return {grpc::StatusCode::UNAVAILABLE,
                        "backing store unavailable"};
            default:
                return {grpc::StatusCode::INTERNAL,
                        std::format("internal error getting partition: {}",
                                    request->partition_id())};
        }
    }

    const auto& config = *result;
    response->set_partition_id(config.partition_id);
    response->set_num_buckets(config.num_buckets);
    response->set_bucket_size_bits(config.bucket_size_bits);
    response->set_super_block_size(config.super_block_size);
    response->set_block_size(config.block_size);
    return grpc::Status::OK;
}

grpc::Status IdAllocatorAdminServiceImpl::DeletePartition(
    grpc::ServerContext* /*context*/,
    const DeletePartitionRequest* request,
    DeletePartitionResponse* /*response*/) {

    auto result = partition_manager_.delete_partition(request->partition_id());
    if (!result) {
        switch (result.error()) {
            case StoreError::unavailable:
                return {grpc::StatusCode::UNAVAILABLE,
                        "backing store unavailable"};
            default:
                // Idempotent: not_found is not an error for delete.
                break;
        }
    }

    return grpc::Status::OK;
}

}  // namespace id_allocator

#include "service/id_allocator_service.h"

#include <format>
#include <string>

namespace id_allocator {

IdAllocatorServiceImpl::IdAllocatorServiceImpl(
    PartitionManager& partition_manager)
    : partition_manager_{partition_manager} {}

grpc::Status IdAllocatorServiceImpl::AllocateBlock(
    grpc::ServerContext* /*context*/,
    const AllocateBlockRequest* request,
    AllocateBlockResponse* response) {

    auto result = partition_manager_.allocate_block(request->partition_id());

    if (!result) {
        const auto& id = request->partition_id();
        switch (result.error()) {
            case StoreError::not_found:
                return {grpc::StatusCode::NOT_FOUND,
                        std::format("partition not found: {}", id)};
            case StoreError::resource_exhausted:
                return {grpc::StatusCode::RESOURCE_EXHAUSTED,
                        std::format("ID space exhausted for partition: {}", id)};
            case StoreError::unavailable:
                return {grpc::StatusCode::UNAVAILABLE,
                        "backing store unavailable"};
            default:
                return {grpc::StatusCode::INTERNAL,
                        std::format("internal error allocating from partition: {}", id)};
        }
    }

    auto [range_start, range_end] = *result;
    response->set_range_start(range_start);
    response->set_range_end(range_end);
    return grpc::Status::OK;
}

}  // namespace id_allocator

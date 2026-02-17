#pragma once

#include <grpcpp/grpcpp.h>

#include "id_allocator.grpc.pb.h"
#include "partition/partition_manager.h"

namespace id_allocator {

class IdAllocatorAdminServiceImpl final : public IdAllocatorAdmin::Service {
public:
    explicit IdAllocatorAdminServiceImpl(PartitionManager& partition_manager);

    grpc::Status CreatePartition(
        grpc::ServerContext* context,
        const CreatePartitionRequest* request,
        CreatePartitionResponse* response) override;

    grpc::Status GetPartition(
        grpc::ServerContext* context,
        const GetPartitionRequest* request,
        GetPartitionResponse* response) override;

    grpc::Status DeletePartition(
        grpc::ServerContext* context,
        const DeletePartitionRequest* request,
        DeletePartitionResponse* response) override;

private:
    PartitionManager& partition_manager_;
};

}  // namespace id_allocator

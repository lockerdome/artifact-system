#pragma once

#include <grpcpp/grpcpp.h>

#include "id_allocator.grpc.pb.h"
#include "partition/partition_manager.h"

namespace id_allocator {

class IdAllocatorServiceImpl final : public IdAllocator::Service {
public:
  explicit IdAllocatorServiceImpl(PartitionManager& partition_manager);

  grpc::Status AllocateBlock(grpc::ServerContext* context, const AllocateBlockRequest* request, AllocateBlockResponse* response) override;

private:
  PartitionManager& partition_manager_;
};

} // namespace id_allocator

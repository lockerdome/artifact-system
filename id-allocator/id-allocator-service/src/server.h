#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "config.h"
#include "partition/partition_manager.h"
#include "service/id_allocator_admin_service.h"
#include "service/id_allocator_service.h"
#include "store/block_store.h"

namespace id_allocator {

class Server {
public:
    Server(ServerConfig config, std::unique_ptr<BlockStore> store);

    /// Initialize the partition manager (loads existing partitions) and start
    /// the gRPC server.  Blocks until shutdown.
    void run();

    /// Trigger graceful shutdown.
    void shutdown();

private:
    ServerConfig config_;
    std::unique_ptr<BlockStore> store_;
    PartitionManager partition_manager_;
    IdAllocatorServiceImpl allocator_service_;
    IdAllocatorAdminServiceImpl admin_service_;
    std::unique_ptr<grpc::Server> grpc_server_;
};

}  // namespace id_allocator

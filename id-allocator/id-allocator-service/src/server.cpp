#include "server.h"

#include <print>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

namespace id_allocator {

Server::Server(ServerConfig config, std::unique_ptr<BlockStore> store)
    : config_{std::move(config)},
      store_{std::move(store)},
      partition_manager_{*store_},
      allocator_service_{partition_manager_},
      admin_service_{partition_manager_} {}

void Server::run() {
    auto init_result = partition_manager_.initialize();
    if (!init_result) {
        std::println(stderr, "Failed to initialize partition manager, aborting");
        std::abort();
    }

    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.listen_address,
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&allocator_service_);
    builder.RegisterService(&admin_service_);

    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
        std::println(stderr, "Failed to start gRPC server on {}",
                     config_.listen_address);
        std::abort();
    }

    std::println("Server listening on {}", config_.listen_address);
    grpc_server_->Wait();
}

void Server::shutdown() {
    if (grpc_server_) {
        grpc_server_->Shutdown();
    }
}

}  // namespace id_allocator

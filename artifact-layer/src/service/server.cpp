#include "service/server.h"

#include <print>

#include "artifact/artifact_store.h"
#include "bootstrap/genesis.h"
#include "grpcpp/health_check_service_interface.h"
#include "grpcpp/server_builder.h"
#include "id/id_allocator_interface.h"
#include "registry/type_registry.h"
#include "service/artifact_service_impl.h"
#include "service/index_service_impl.h"
#include "service/snapshot_transaction_service_impl.h"
#include "service/type_registry_service_impl.h"
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"

namespace artifact_system::service {

struct ArtifactLayerServer::Impl {
  ServerConfig config;

  // Core dependencies (owned)
  std::unique_ptr<MemoryStorage> storage;
  std::unique_ptr<MockIdAllocator> id_allocator;
  std::unique_ptr<transaction::TransactionManager> txn_manager;
  std::unique_ptr<artifact::ArtifactStore> artifact_store;
  std::unique_ptr<registry::TypeRegistry> type_registry;

  // Service implementations (owned)
  std::unique_ptr<SnapshotTransactionServiceImpl> snapshot_txn_service;
  std::unique_ptr<ArtifactServiceImpl> artifact_service;
  std::unique_ptr<IndexServiceImpl> index_service;
  std::unique_ptr<TypeRegistryServiceImpl> type_registry_service;

  // gRPC server
  std::unique_ptr<grpc::Server> server;
  int selected_port = 0;
};

ArtifactLayerServer::ArtifactLayerServer(const ServerConfig& config) : impl_(std::make_unique<Impl>()) {
  impl_->config = config;
}

ArtifactLayerServer::~ArtifactLayerServer() = default;

absl::Status ArtifactLayerServer::Initialize() {
  // 1. Create storage
  impl_->storage = std::make_unique<MemoryStorage>();

  // 2. Run genesis bootstrap
  auto genesis_result = bootstrap::RunGenesis(impl_->storage.get());
  if (!genesis_result.ok()) {
    return genesis_result.status();
  }

  // 3. Create ID allocator (starting after pre-allocated genesis IDs)
  impl_->id_allocator = std::make_unique<MockIdAllocator>(bootstrap::GenesisIds::kFirstUserAllocatableId);

  // 4. Create transaction manager
  impl_->txn_manager = std::make_unique<transaction::TransactionManager>(impl_->storage.get());

  // 5. Create artifact store
  artifact::ArtifactStore::Options store_options{
      .index_def_ids_by_key_type = genesis_result->index_def_ids_by_key_type,
  };
  impl_->artifact_store = std::make_unique<artifact::ArtifactStore>(impl_->storage.get(), impl_->txn_manager.get(), impl_->id_allocator.get(), store_options);

  // 6. Create type registry
  impl_->type_registry = std::make_unique<registry::TypeRegistry>(impl_->storage.get(), impl_->txn_manager.get(), impl_->id_allocator.get(),
                                                                  genesis_result->index_def_ids_by_key_type);

  // 7. Create service implementations
  impl_->snapshot_txn_service = std::make_unique<SnapshotTransactionServiceImpl>(impl_->txn_manager.get());
  impl_->artifact_service = std::make_unique<ArtifactServiceImpl>(impl_->artifact_store.get());
  impl_->index_service = std::make_unique<IndexServiceImpl>(impl_->storage.get(), impl_->txn_manager.get(), impl_->type_registry.get());
  impl_->type_registry_service = std::make_unique<TypeRegistryServiceImpl>(impl_->type_registry.get(), impl_->artifact_store.get());

  return absl::OkStatus();
}

void ArtifactLayerServer::Start() {
  grpc::EnableDefaultHealthCheckService(true);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(impl_->config.listen_address, grpc::InsecureServerCredentials(), &impl_->selected_port);

  builder.RegisterService(impl_->snapshot_txn_service.get());
  builder.RegisterService(impl_->artifact_service.get());
  builder.RegisterService(impl_->index_service.get());
  builder.RegisterService(impl_->type_registry_service.get());

  impl_->server = builder.BuildAndStart();
  if (!impl_->server) {
    std::println(stderr, "Failed to start gRPC server on {}", impl_->config.listen_address);
    return;
  }
  std::println("Artifact layer server listening on {}", impl_->config.listen_address);

  impl_->server->Wait();
}

void ArtifactLayerServer::Shutdown() {
  if (impl_->server) {
    impl_->server->Shutdown();
  }
}

int ArtifactLayerServer::port() const {
  return impl_->selected_port;
}

} // namespace artifact_system::service

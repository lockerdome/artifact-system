#pragma once

#include "artifact_service.grpc.pb.h"
#include "registry/type_registry.h"
#include "storage/storage_interface.h"
#include "transaction/transaction_manager.h"

namespace artifact_system::service {

class IndexServiceImpl final : public IndexService::Service {
public:
  IndexServiceImpl(StorageInterface* storage, transaction::TransactionManager* txn_manager, registry::TypeRegistry* registry);

  grpc::Status FetchIndex(grpc::ServerContext* context, const FetchIndexRequest* request, FetchIndexResponse* response) override;

private:
  StorageInterface* storage_;
  transaction::TransactionManager* txn_manager_;
  registry::TypeRegistry* registry_;
};

} // namespace artifact_system::service

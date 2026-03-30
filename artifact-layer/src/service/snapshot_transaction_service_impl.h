#ifndef ARTIFACT_SYSTEM_SERVICE_SNAPSHOT_TRANSACTION_SERVICE_IMPL_H_
#define ARTIFACT_SYSTEM_SERVICE_SNAPSHOT_TRANSACTION_SERVICE_IMPL_H_

#include "artifact_service.grpc.pb.h"
#include "transaction/transaction_manager.h"

namespace artifact_system::service {

class SnapshotTransactionServiceImpl final : public artifact_system::SnapshotTransactionService::Service {
public:
  explicit SnapshotTransactionServiceImpl(transaction::TransactionManager* transaction_manager);

  grpc::Status CreateSnapshot(grpc::ServerContext* context, const CreateSnapshotRequest* request, CreateSnapshotResponse* response) override;

  grpc::Status CreateTransaction(grpc::ServerContext* context, const CreateTransactionRequest* request, CreateTransactionResponse* response) override;

  grpc::Status CommitTransaction(grpc::ServerContext* context, const CommitTransactionRequest* request, CommitTransactionResponse* response) override;

  grpc::Status RollbackTransaction(grpc::ServerContext* context, const RollbackTransactionRequest* request, RollbackTransactionResponse* response) override;

private:
  transaction::TransactionManager* transaction_manager_;
};

} // namespace artifact_system::service

#endif // ARTIFACT_SYSTEM_SERVICE_SNAPSHOT_TRANSACTION_SERVICE_IMPL_H_

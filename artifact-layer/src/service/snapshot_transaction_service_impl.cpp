#include "service/snapshot_transaction_service_impl.h"

#include "service/grpc_error_util.h"

namespace artifact_system::service {

SnapshotTransactionServiceImpl::SnapshotTransactionServiceImpl(
    transaction::TransactionManager* transaction_manager)
    : transaction_manager_(transaction_manager) {}

grpc::Status SnapshotTransactionServiceImpl::CreateSnapshot(
    grpc::ServerContext* context,
    const CreateSnapshotRequest* request,
    CreateSnapshotResponse* response) {
  std::optional<std::string> parent_transaction_id;
  if (request->has_parent_transaction_id()) {
    parent_transaction_id = request->parent_transaction_id();
  }

  auto result = transaction_manager_->CreateSnapshot(parent_transaction_id);
  if (!result.ok()) {
    if (absl::IsNotFound(result.status())) {
      SnapshotTransactionError error;
      error.set_category(SnapshotTransactionError::PARENT_NOT_FOUND);
      error.set_description(std::string(result.status().message()));
      error.set_id(request->parent_transaction_id());
      auto status_with_detail = MakeStatusWithDetail(
          absl::StatusCode::kNotFound,
          std::string(result.status().message()), error);
      return AbslToGrpcStatus(status_with_detail);
    }
    return AbslToGrpcStatus(result.status());
  }

  response->set_snapshot_id(*result);
  return grpc::Status::OK;
}

grpc::Status SnapshotTransactionServiceImpl::CreateTransaction(
    grpc::ServerContext* context,
    const CreateTransactionRequest* request,
    CreateTransactionResponse* response) {
  std::optional<std::string> parent_snapshot_id;
  std::optional<std::string> parent_transaction_id;

  switch (request->parent_case()) {
    case CreateTransactionRequest::kParentSnapshotId:
      parent_snapshot_id = request->parent_snapshot_id();
      break;
    case CreateTransactionRequest::kParentTransactionId:
      parent_transaction_id = request->parent_transaction_id();
      break;
    case CreateTransactionRequest::PARENT_NOT_SET:
      break;
  }

  auto result = transaction_manager_->CreateTransaction(
      parent_snapshot_id, parent_transaction_id);
  if (!result.ok()) {
    if (absl::IsNotFound(result.status())) {
      SnapshotTransactionError error;
      error.set_category(SnapshotTransactionError::PARENT_NOT_FOUND);
      error.set_description(std::string(result.status().message()));
      if (parent_snapshot_id.has_value()) {
        error.set_id(*parent_snapshot_id);
      } else if (parent_transaction_id.has_value()) {
        error.set_id(*parent_transaction_id);
      }
      auto status_with_detail = MakeStatusWithDetail(
          absl::StatusCode::kNotFound,
          std::string(result.status().message()), error);
      return AbslToGrpcStatus(status_with_detail);
    }
    return AbslToGrpcStatus(result.status());
  }

  response->set_transaction_id(*result);
  return grpc::Status::OK;
}

grpc::Status SnapshotTransactionServiceImpl::CommitTransaction(
    grpc::ServerContext* context,
    const CommitTransactionRequest* request,
    CommitTransactionResponse* response) {
  auto result = transaction_manager_->CommitTransaction(
      request->transaction_id());
  if (!result.ok()) {
    if (absl::IsNotFound(result.status())) {
      SnapshotTransactionError error;
      error.set_category(SnapshotTransactionError::TRANSACTION_NOT_FOUND);
      error.set_description(std::string(result.status().message()));
      error.set_id(request->transaction_id());
      auto status_with_detail = MakeStatusWithDetail(
          absl::StatusCode::kNotFound,
          std::string(result.status().message()), error);
      return AbslToGrpcStatus(status_with_detail);
    }
    return AbslToGrpcStatus(result.status());
  }

  if (auto* conflict =
          std::get_if<transaction::TransactionManager::CommitConflict>(
              &*result)) {
    auto status_with_detail = MakeStatusWithDetail(
        absl::StatusCode::kAborted,
        "commit conflict for transaction: " + request->transaction_id(),
        conflict->detail);
    return AbslToGrpcStatus(status_with_detail);
  }

  auto& success =
      std::get<transaction::TransactionManager::CommitSuccess>(*result);
  response->set_snapshot_id(success.snapshot_id);
  return grpc::Status::OK;
}

grpc::Status SnapshotTransactionServiceImpl::RollbackTransaction(
    grpc::ServerContext* context,
    const RollbackTransactionRequest* request,
    RollbackTransactionResponse* response) {
  auto status = transaction_manager_->RollbackTransaction(
      request->transaction_id());
  if (!status.ok()) {
    if (absl::IsNotFound(status)) {
      SnapshotTransactionError error;
      error.set_category(SnapshotTransactionError::TRANSACTION_NOT_FOUND);
      error.set_description(std::string(status.message()));
      error.set_id(request->transaction_id());
      auto status_with_detail = MakeStatusWithDetail(
          absl::StatusCode::kNotFound,
          std::string(status.message()), error);
      return AbslToGrpcStatus(status_with_detail);
    }
    return AbslToGrpcStatus(status);
  }

  return grpc::Status::OK;
}

}  // namespace artifact_system::service

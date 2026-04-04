#include "transaction/context_ref_resolver.h"

#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"

namespace artifact_system::transaction {

absl::Status MakeSnapshotTransactionNotFoundStatus(SnapshotTransactionError::Category category, const std::string& id) {
  SnapshotTransactionError detail;
  detail.set_category(category);
  if (category == SnapshotTransactionError::SNAPSHOT_NOT_FOUND) {
    detail.set_description(absl::StrCat("snapshot not found: ", id));
  } else {
    detail.set_description(absl::StrCat("transaction not found: ", id));
  }
  detail.set_id(id);

  absl::Status status = absl::NotFoundError(detail.description());
  std::string serialized;
  detail.SerializeToString(&serialized);
  status.SetPayload("type.googleapis.com/artifact_system.SnapshotTransactionError", absl::Cord(serialized));
  return status;
}

absl::Status RewriteSnapshotTransactionNotFound(const absl::Status& status, SnapshotTransactionError::Category category, const std::string& id) {
  if (absl::IsNotFound(status)) {
    return MakeSnapshotTransactionNotFoundStatus(category, id);
  }
  return status;
}

absl::StatusOr<std::string> ResolveReadRef(StorageInterface* storage, TransactionManager* transaction_manager, const ReadContext& context) {
  if (context.has_snapshot_id()) {
    auto meta_or = transaction_manager->GetSnapshotMetadata(context.snapshot_id());
    if (!meta_or.ok()) {
      return RewriteSnapshotTransactionNotFound(meta_or.status(), SnapshotTransactionError::SNAPSHOT_NOT_FOUND, context.snapshot_id());
    }
    return context.snapshot_id();
  }
  if (context.has_transaction_id()) {
    auto meta_or = transaction_manager->GetTransactionMetadata(context.transaction_id());
    if (!meta_or.ok()) {
      return RewriteSnapshotTransactionNotFound(meta_or.status(), SnapshotTransactionError::TRANSACTION_NOT_FOUND, context.transaction_id());
    }
    return context.transaction_id();
  }
  return std::string(storage->GetCanonicalBranch());
}

absl::StatusOr<std::string> ResolveWriteRef(StorageInterface* storage, TransactionManager* transaction_manager,
                                            const std::optional<std::string>& transaction_id) {
  if (!transaction_id.has_value()) {
    return std::string(storage->GetCanonicalBranch());
  }
  auto meta_or = transaction_manager->GetTransactionMetadata(*transaction_id);
  if (!meta_or.ok()) {
    return RewriteSnapshotTransactionNotFound(meta_or.status(), SnapshotTransactionError::TRANSACTION_NOT_FOUND, *transaction_id);
  }
  return *transaction_id;
}

} // namespace artifact_system::transaction

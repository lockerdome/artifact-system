#pragma once

#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "artifact_service.pb.h"
#include "storage/storage_interface.h"
#include "transaction/transaction_manager.h"

namespace artifact_system::transaction {

absl::Status MakeSnapshotTransactionNotFoundStatus(SnapshotTransactionError::Category category, const std::string& id);

absl::Status RewriteSnapshotTransactionNotFound(const absl::Status& status, SnapshotTransactionError::Category category, const std::string& id);

absl::StatusOr<std::string> ResolveReadRef(StorageInterface* storage, TransactionManager* transaction_manager, const ReadContext& context);

absl::StatusOr<std::string> ResolveWriteRef(StorageInterface* storage, TransactionManager* transaction_manager,
                                            const std::optional<std::string>& transaction_id);

} // namespace artifact_system::transaction

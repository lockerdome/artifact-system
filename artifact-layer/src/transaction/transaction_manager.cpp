#include "transaction/transaction_manager.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "artifact/artifact_store.h"
#include "artifact_types.pb.h"
#include "index/index_conflict_resolver.h"
#include "transaction/conflict_resolver.h"
#include "util/uuid.h"

namespace artifact_system::transaction {

TransactionManager::TransactionManager(StorageInterface* storage) : TransactionManager(storage, Options{}) {
}

TransactionManager::TransactionManager(StorageInterface* storage, Options options) : storage_(storage), options_(std::move(options)) {
  const bool using_default_classifier = options_.path_conflict_classifier == nullptr;
  if (using_default_classifier) {
    options_.path_conflict_classifier = index::BuildIndexPathConflictClassifier(storage_);
  }
  if (using_default_classifier && options_.retry_conflict_resolver == nullptr) {
    options_.retry_conflict_resolver = index::BuildDeterministicIndexRetryConflictResolver(storage_);
  }
  if (options_.sleep_for == nullptr) {
    options_.sleep_for = [](absl::Duration d) { absl::SleepFor(d); };
  }
  if (options_.on_cleanup_failure == nullptr) {
    options_.on_cleanup_failure = [](const absl::Status&) {};
  }
}

void TransactionManager::SetCommitRecordConfig(CommitRecordConfig config) {
  options_.commit_record_config = std::move(config);
}

std::string TransactionManager::GenerateBranchName() {
  return absl::StrCat("txn-", util::GenerateUUID());
}

std::string TransactionManager::TxnMetaPath(const std::string& transaction_id) {
  return absl::StrCat("_txn_meta/", transaction_id);
}

absl::Status TransactionNotFoundStatus(const std::string& transaction_id) {
  return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
}

absl::Status ParentTransactionNotFoundStatus(const std::string& transaction_id) {
  return absl::NotFoundError(absl::StrCat("parent transaction not found: ", transaction_id));
}

absl::Status SnapshotNotFoundStatus(const std::string& snapshot_id) {
  return absl::NotFoundError(absl::StrCat("snapshot not found: ", snapshot_id));
}

absl::Status MissingTransactionMetadataStatus(const std::string& transaction_id) {
  return absl::FailedPreconditionError(absl::StrCat("transaction metadata missing for branch: ", transaction_id));
}

absl::Status RewriteNotFound(const absl::Status& status, const absl::Status& replacement) {
  if (absl::IsNotFound(status)) {
    return replacement;
  }
  return status;
}

absl::Status TransactionManager::WriteTxnMeta(const std::string& branch, const TransactionRecord& record) {
  std::string data = absl::StrCat(record.parent_transaction_id.value_or(""), "\n", record.parent_snapshot_id.value_or(""), "\n", record.depth);
  auto put_status = storage_->PutObject(branch, TxnMetaPath(branch), data);
  if (!put_status.ok())
    return put_status;
  auto commit_or = storage_->Commit(branch, "txn metadata");
  if (!commit_or.ok())
    return commit_or.status();
  return absl::OkStatus();
}

absl::StatusOr<TransactionManager::TransactionRecord> TransactionManager::LoadTxnMeta(const std::string& branch) const {
  auto data_or = storage_->GetObject(branch, TxnMetaPath(branch));
  if (!data_or.ok())
    return data_or.status();

  const std::string& data = *data_or;
  std::vector<std::string> lines = absl::StrSplit(data, '\n');
  if (lines.size() < 3) {
    return absl::InternalError(absl::StrCat("malformed txn metadata on branch: ", branch));
  }

  TransactionRecord record;
  if (!lines[0].empty())
    record.parent_transaction_id = lines[0];
  if (!lines[1].empty())
    record.parent_snapshot_id = lines[1];
  uint32_t depth = 0;
  if (!absl::SimpleAtoi(lines[2], &depth)) {
    return absl::InternalError(absl::StrCat("invalid depth in txn metadata: ", lines[2]));
  }
  record.depth = depth;
  return record;
}

absl::Status TransactionManager::CleanupTxnMeta(const std::string& branch) {
  auto del_status = storage_->DeleteObject(branch, TxnMetaPath(branch));
  if (!del_status.ok())
    return del_status;
  auto commit_or = storage_->Commit(branch, "cleanup txn metadata");
  if (!commit_or.ok())
    return commit_or.status();
  return absl::OkStatus();
}

absl::StatusOr<TransactionManager::TransactionRecord> TransactionManager::LoadTransactionRecordFromStorage(const std::string& transaction_id) const {
  auto branch_head_or = storage_->GetBranchHead(transaction_id);
  if (!branch_head_or.ok()) {
    return RewriteNotFound(branch_head_or.status(), TransactionNotFoundStatus(transaction_id));
  }

  auto record_or = LoadTxnMeta(transaction_id);
  if (!record_or.ok()) {
    if (absl::IsNotFound(record_or.status())) {
      return MissingTransactionMetadataStatus(transaction_id);
    }
    return record_or.status();
  }

  return *record_or;
}

absl::StatusOr<TransactionManager::TransactionRecord> TransactionManager::ResolveTransaction(const std::string& transaction_id) {
  auto cache_it = transactions_.find(transaction_id);
  if (cache_it != transactions_.end()) {
    return cache_it->second;
  }

  auto record_or = LoadTransactionRecordFromStorage(transaction_id);
  if (!record_or.ok()) {
    return record_or.status();
  }

  transactions_[transaction_id] = *record_or;
  return *record_or;
}

absl::StatusOr<std::string> TransactionManager::CreateSnapshot(std::optional<std::string> parent_transaction_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  std::string commit_id;
  std::optional<std::string> source_transaction_id;

  if (parent_transaction_id.has_value()) {
    const auto& parent_id = *parent_transaction_id;

    // Validate that the parent is a transaction (branch), not a snapshot (commit).
    auto branch_head_or = storage_->GetBranchHead(parent_id);
    if (!branch_head_or.ok()) {
      if (absl::IsNotFound(branch_head_or.status())) {
        // Not a branch — check if it's a commit (snapshot).
        auto commit_exists_or = storage_->CommitExists(parent_id);
        if (commit_exists_or.ok() && *commit_exists_or) {
          return absl::InvalidArgumentError(absl::StrCat("parent id references snapshot; expected transaction id: ", parent_id));
        }
        return TransactionNotFoundStatus(parent_id);
      }
      return branch_head_or.status();
    }

    commit_id = *branch_head_or;
    source_transaction_id = parent_id;
  } else {
    auto branch_head_or = storage_->GetBranchHead(storage_->GetCanonicalBranch());
    if (!branch_head_or.ok()) {
      return branch_head_or.status();
    }
    commit_id = *branch_head_or;
  }

  // Cache the source_transaction_id (informational, best-effort).
  snapshot_source_cache_[commit_id] = SnapshotSource{
      .source_transaction_id = source_transaction_id,
  };
  return commit_id;
}

absl::StatusOr<std::string> TransactionManager::CreateTransaction(std::optional<std::string> parent_snapshot_id,
                                                                  std::optional<std::string> parent_transaction_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  if (parent_snapshot_id.has_value() && parent_transaction_id.has_value()) {
    return absl::InvalidArgumentError("cannot specify both parent_snapshot_id and parent_transaction_id");
  }

  std::optional<std::string> resolved_parent_transaction_id;
  std::optional<std::string> resolved_parent_snapshot_id;
  uint32_t depth = 0;
  std::string base_commit_id;

  if (parent_transaction_id.has_value()) {
    const auto& parent_id = *parent_transaction_id;

    // Validate parent transaction via Storage Layer.
    auto branch_head_or = storage_->GetBranchHead(parent_id);
    if (!branch_head_or.ok()) {
      return RewriteNotFound(branch_head_or.status(), ParentTransactionNotFoundStatus(parent_id));
    }

    resolved_parent_transaction_id = parent_id;
    base_commit_id = *branch_head_or;

    // Get depth from cache or metadata.
    auto parent_record_or = ResolveTransaction(parent_id);
    if (!parent_record_or.ok()) {
      return parent_record_or.status();
    }
    depth = parent_record_or->depth + 1;
  } else if (parent_snapshot_id.has_value()) {
    const auto& parent_id = *parent_snapshot_id;

    // Validate snapshot via Storage Layer: a snapshot_id IS a commit hash.
    auto commit_exists_or = storage_->CommitExists(parent_id);
    if (!commit_exists_or.ok())
      return commit_exists_or.status();
    if (!*commit_exists_or) {
      return SnapshotNotFoundStatus(parent_id);
    }

    resolved_parent_snapshot_id = parent_id;
    base_commit_id = parent_id;
  } else {
    auto branch_head_or = storage_->GetBranchHead(storage_->GetCanonicalBranch());
    if (!branch_head_or.ok()) {
      return branch_head_or.status();
    }
    base_commit_id = *branch_head_or;
  }

  const std::string branch_name = GenerateBranchName();

  auto branch_or = storage_->CreateBranch(branch_name, base_commit_id);
  if (!branch_or.ok()) {
    return branch_or.status();
  }

  // Store transaction metadata on the branch for cross-instance access.
  TransactionRecord record{
      .parent_transaction_id = resolved_parent_transaction_id,
      .parent_snapshot_id = resolved_parent_snapshot_id,
      .depth = depth,
      .child_transaction_ids = {},
  };

  auto meta_status = WriteTxnMeta(branch_name, record);
  if (!meta_status.ok()) {
    // Best effort cleanup: delete the branch if metadata write fails.
    storage_->DeleteBranch(branch_name).IgnoreError();
    return meta_status;
  }

  // Cache the transaction record locally.
  transactions_[branch_name] = record;

  if (resolved_parent_transaction_id.has_value()) {
    auto parent_cache_it = transactions_.find(*resolved_parent_transaction_id);
    if (parent_cache_it != transactions_.end()) {
      parent_cache_it->second.child_transaction_ids.insert(branch_name);
    }
  }

  return branch_name;
}

absl::Status TransactionManager::RemoveTransactionLocked(const std::string& transaction_id) {
  auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return TransactionNotFoundStatus(transaction_id);
  }

  if (transaction_it->second.parent_transaction_id.has_value()) {
    auto parent_it = transactions_.find(*transaction_it->second.parent_transaction_id);
    if (parent_it != transactions_.end()) {
      parent_it->second.child_transaction_ids.erase(transaction_id);
    }
  }

  transactions_.erase(transaction_it);
  return absl::OkStatus();
}

absl::StatusOr<TransactionManager::CommitResult> TransactionManager::CommitTransaction(const std::string& transaction_id) {
  return CommitTransactionImpl(transaction_id, /*write_commit_record=*/true);
}

absl::StatusOr<TransactionManager::CommitResult> TransactionManager::CommitTransactionImpl(const std::string& transaction_id, bool write_commit_record) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  auto record_or = ResolveTransaction(transaction_id);
  if (!record_or.ok())
    return record_or.status();

  const TransactionRecord transaction = *record_or;
  if (!transaction.child_transaction_ids.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
  }

  if (options_.conflict_options.max_attempts == 0) {
    return absl::InvalidArgumentError("max attempts must be greater than zero");
  }

  std::string target_branch;
  if (transaction.parent_transaction_id.has_value()) {
    // Validate parent still exists.
    auto parent_head_or = storage_->GetBranchHead(*transaction.parent_transaction_id);
    if (!parent_head_or.ok()) {
      return RewriteNotFound(parent_head_or.status(), ParentTransactionNotFoundStatus(*transaction.parent_transaction_id));
    }
    target_branch = *transaction.parent_transaction_id;
  } else {
    target_branch = storage_->GetCanonicalBranch();
  }

  // ── Write TransactionCommitRecord via the internal bypass ArtifactStore.
  if (write_commit_record && options_.commit_record_config.has_value()) {
    const auto& cfg = *options_.commit_record_config;

    TransactionCommitRecord record;
    record.set_transaction_id(transaction_id);
    record.set_committed_at(absl::ToUnixSeconds(absl::Now()));

    lock.unlock();

    auto create_or = cfg.artifact_store->CreateArtifact(cfg.version_def_id, record.SerializeAsString(), transaction_id);
    if (!create_or.ok()) {
      return create_or.status();
    }

    lock.lock();

    // Re-validate transaction state after releasing and re-acquiring the lock.
    auto branch_head_or = storage_->GetBranchHead(transaction_id);
    if (!branch_head_or.ok()) {
      return RewriteNotFound(branch_head_or.status(), TransactionNotFoundStatus(transaction_id));
    }
  }

  // Clean up transaction metadata before merge to avoid polluting the target branch.
  // If the merge fails, metadata is restored so the transaction remains usable.
  auto cleanup_status = CleanupTxnMeta(transaction_id);
  if (!cleanup_status.ok()) {
    return cleanup_status;
  }

  // Restore metadata on any exit path that does not delete the branch.
  auto restore_meta = [&]() { (void)WriteTxnMeta(transaction_id, transaction); };

  for (uint32_t attempts_performed = 1; attempts_performed <= options_.conflict_options.max_attempts; ++attempts_performed) {
    auto merge_or = storage_->Merge(transaction_id, target_branch);
    if (!merge_or.ok()) {
      restore_meta();
      return merge_or.status();
    }

    const MergeResult& merge_result = *merge_or;
    if (merge_result.IsConflict()) {
      const RetryDecision retry_decision =
          EvaluateRetryDecision(merge_result.GetConflict(), attempts_performed, options_.conflict_options, options_.path_conflict_classifier);
      artifact_system::CommitConflict detail =
          BuildCommitConflict(merge_result.GetConflict(), attempts_performed, options_.conflict_options, options_.path_conflict_classifier);

      if (!retry_decision.retryable) {
        restore_meta();
        return CommitConflict{
            .transaction_id = transaction_id,
            .conflict = merge_result.GetConflict(),
            .detail = detail,
        };
      }

      if (options_.retry_conflict_resolver != nullptr) {
        RetryResolutionContext resolution_context;
        resolution_context.merge_conflict = merge_result.GetConflict();
        resolution_context.source_branch = transaction_id;
        resolution_context.target_branch = target_branch;
        resolution_context.attempts_performed = attempts_performed;
        auto resolved_or = options_.retry_conflict_resolver(resolution_context);
        if (!resolved_or.ok()) {
          restore_meta();
          return resolved_or.status();
        }
        if (!*resolved_or) {
          detail.set_retryable(false);
          restore_meta();
          return CommitConflict{
              .transaction_id = transaction_id,
              .conflict = merge_result.GetConflict(),
              .detail = detail,
          };
        }
      }

      lock.unlock();
      options_.sleep_for(ComputeBackoffWithJitter(attempts_performed - 1, options_.conflict_options));
      lock.lock();

      // Re-validate transaction exists after re-acquiring lock.
      auto branch_exists_or = storage_->BranchExists(transaction_id);
      if (!branch_exists_or.ok()) {
        restore_meta();
        return branch_exists_or.status();
      }
      if (!*branch_exists_or) {
        restore_meta();
        return TransactionNotFoundStatus(transaction_id);
      }
      // Re-check children from local cache (best-effort).
      auto cache_it = transactions_.find(transaction_id);
      if (cache_it != transactions_.end() && !cache_it->second.child_transaction_ids.empty()) {
        restore_meta();
        return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
      }
      continue;
    }

    auto remove_status = RemoveTransactionLocked(transaction_id);
    if (!remove_status.ok()) {
      // Ignore NOT_FOUND — transaction may have been removed from cache by another path.
      if (!absl::IsNotFound(remove_status))
        return remove_status;
    }
    auto delete_status = storage_->DeleteBranch(transaction_id);
    if (!delete_status.ok()) {
      options_.on_cleanup_failure(delete_status);
    }

    // Cache snapshot source (informational, best-effort).
    const std::string& new_snapshot_id = merge_result.GetSuccess().commit_id;
    snapshot_source_cache_[new_snapshot_id] = SnapshotSource{
        .source_transaction_id = transaction_id,
    };

    return CommitSuccess{
        .transaction_id = transaction_id,
        .commit_id = merge_result.GetSuccess().commit_id,
        .snapshot_id = new_snapshot_id,
    };
  }

  restore_meta();
  return absl::InternalError("commit loop exited unexpectedly");
}

absl::Status TransactionManager::RollbackTransaction(const std::string& transaction_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  // Validate transaction exists via Storage Layer.
  auto branch_exists_or = storage_->BranchExists(transaction_id);
  if (!branch_exists_or.ok())
    return branch_exists_or.status();
  if (!*branch_exists_or) {
    return TransactionNotFoundStatus(transaction_id);
  }

  // Check for children in local cache (best-effort).
  auto cache_it = transactions_.find(transaction_id);
  if (cache_it != transactions_.end() && !cache_it->second.child_transaction_ids.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
  }

  // The transaction_id IS the branch name.
  auto delete_status = storage_->DeleteBranch(transaction_id);
  if (!delete_status.ok()) {
    return delete_status;
  }

  // Remove from local cache.
  auto remove_status = RemoveTransactionLocked(transaction_id);
  if (!remove_status.ok() && !absl::IsNotFound(remove_status)) {
    return remove_status;
  }
  return absl::OkStatus();
}

absl::StatusOr<TransactionManager::CommitResult> TransactionManager::RunImplicitTransaction(const ImplicitTransactionCallback& callback,
                                                                                            std::optional<std::string> parent_id) {
  if (callback == nullptr) {
    return absl::InvalidArgumentError("callback is null");
  }

  auto transaction_id_or = CreateTransaction(/*parent_snapshot_id=*/parent_id, /*parent_transaction_id=*/std::nullopt);
  if (!transaction_id_or.ok()) {
    return transaction_id_or.status();
  }
  const std::string& transaction_id = *transaction_id_or;

  const absl::Status callback_status = callback(transaction_id);
  if (!callback_status.ok()) {
    const absl::Status rollback_status = RollbackTransaction(transaction_id);
    if (!rollback_status.ok()) {
      return absl::InternalError(absl::StrCat("callback failed and rollback failed: ", rollback_status.message()));
    }
    return callback_status;
  }

  // The callback stages writes directly on the transaction branch.
  // Commit them before the merge.
  auto staged_commit_or = storage_->Commit(transaction_id, absl::StrCat("implicit transaction ", transaction_id));
  if (!staged_commit_or.ok()) {
    return staged_commit_or.status();
  }

  auto commit_or = CommitTransactionImpl(transaction_id, /*write_commit_record=*/false);
  if (!commit_or.ok()) {
    const absl::Status rollback_status = RollbackTransaction(transaction_id);
    if (!rollback_status.ok()) {
      return absl::InternalError(absl::StrCat("commit failed (", commit_or.status().message(), ") and rollback failed: ", rollback_status.message()));
    }
    return commit_or.status();
  }

  if (std::holds_alternative<CommitConflict>(*commit_or)) {
    const absl::Status rollback_status = RollbackTransaction(transaction_id);
    if (!rollback_status.ok()) {
      return absl::InternalError(absl::StrCat("implicit transaction commit conflicted and rollback failed: ", rollback_status.message()));
    }
  }

  return *commit_or;
}

absl::StatusOr<TransactionManager::SnapshotMetadata> TransactionManager::GetSnapshotMetadata(const std::string& snapshot_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // A snapshot_id IS a commit hash. Validate via Storage Layer.
  auto commit_exists_or = storage_->CommitExists(snapshot_id);
  if (!commit_exists_or.ok())
    return commit_exists_or.status();
  if (!*commit_exists_or) {
    return SnapshotNotFoundStatus(snapshot_id);
  }

  // source_transaction_id is informational, from local cache (best-effort).
  std::optional<std::string> source_transaction_id;
  auto cache_it = snapshot_source_cache_.find(snapshot_id);
  if (cache_it != snapshot_source_cache_.end()) {
    source_transaction_id = cache_it->second.source_transaction_id;
  }

  return SnapshotMetadata{
      .snapshot_id = snapshot_id,
      .source_transaction_id = source_transaction_id,
  };
}

absl::StatusOr<TransactionManager::TransactionMetadata> TransactionManager::GetTransactionMetadata(const std::string& transaction_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check cache first.
  auto cache_it = transactions_.find(transaction_id);
  if (cache_it != transactions_.end()) {
    return TransactionMetadata{
        .transaction_id = transaction_id,
        .parent_transaction_id = cache_it->second.parent_transaction_id,
        .parent_snapshot_id = cache_it->second.parent_snapshot_id,
        .depth = cache_it->second.depth,
    };
  }

  auto record_or = LoadTransactionRecordFromStorage(transaction_id);
  if (!record_or.ok()) {
    return record_or.status();
  }

  return TransactionMetadata{
      .transaction_id = transaction_id,
      .parent_transaction_id = record_or->parent_transaction_id,
      .parent_snapshot_id = record_or->parent_snapshot_id,
      .depth = record_or->depth,
  };
}

} // namespace artifact_system::transaction

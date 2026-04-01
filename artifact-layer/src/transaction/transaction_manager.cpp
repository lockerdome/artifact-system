#include "transaction/transaction_manager.h"

#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "artifact/artifact_store.h"
#include "artifact_types.pb.h"
#include "index/index_conflict_resolver.h"
#include "transaction/conflict_resolver.h"

namespace artifact_system::transaction {
namespace {

std::string GenerateUUID() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  const uint64_t hi = rng();
  const uint64_t lo = rng();
  return absl::StrFormat("%016x%016x", hi, lo);
}

} // namespace

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
  return absl::StrCat("txn-", GenerateUUID());
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
    const auto transaction_it = transactions_.find(parent_id);
    if (transaction_it == transactions_.end()) {
      if (snapshots_.contains(parent_id)) {
        return absl::InvalidArgumentError(absl::StrCat("parent id references snapshot; expected transaction id: ", parent_id));
      }
      return absl::NotFoundError(absl::StrCat("transaction not found: ", parent_id));
    }

    // The transaction_id IS the branch name.
    auto branch_head_or = storage_->GetBranchHead(parent_id);
    if (!branch_head_or.ok()) {
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

  // The snapshot_id IS the commit hash.
  snapshots_[commit_id] = SnapshotSource{
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
    const auto transaction_it = transactions_.find(parent_id);
    if (transaction_it == transactions_.end()) {
      return absl::NotFoundError(absl::StrCat("parent transaction not found: ", parent_id));
    }
    resolved_parent_transaction_id = parent_id;
    depth = transaction_it->second.depth + 1;

    // The transaction_id IS the branch name.
    auto branch_head_or = storage_->GetBranchHead(parent_id);
    if (!branch_head_or.ok()) {
      return branch_head_or.status();
    }
    base_commit_id = *branch_head_or;
  } else if (parent_snapshot_id.has_value()) {
    const auto& parent_id = *parent_snapshot_id;
    const auto snapshot_it = snapshots_.find(parent_id);
    if (snapshot_it == snapshots_.end()) {
      return absl::NotFoundError(absl::StrCat("parent snapshot not found: ", parent_id));
    }
    resolved_parent_snapshot_id = parent_id;
    // The snapshot_id IS the commit hash.
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

  // The transaction_id IS the branch name.
  transactions_[branch_name] = TransactionRecord{
      .parent_transaction_id = resolved_parent_transaction_id,
      .parent_snapshot_id = resolved_parent_snapshot_id,
      .depth = depth,
      .child_transaction_ids = {},
  };

  if (resolved_parent_transaction_id.has_value()) {
    transactions_.at(*resolved_parent_transaction_id).child_transaction_ids.insert(branch_name);
  }

  return branch_name;
}

absl::Status TransactionManager::RemoveTransactionLocked(const std::string& transaction_id) {
  auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
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

  auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
  }

  const TransactionRecord transaction = transaction_it->second;
  if (!transaction.child_transaction_ids.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
  }

  if (options_.conflict_options.max_attempts == 0) {
    return absl::InvalidArgumentError("max attempts must be greater than zero");
  }

  std::string target_branch;
  if (transaction.parent_transaction_id.has_value()) {
    auto parent_it = transactions_.find(*transaction.parent_transaction_id);
    if (parent_it == transactions_.end()) {
      return absl::NotFoundError(absl::StrCat("parent transaction not found: ", *transaction.parent_transaction_id));
    }
    // The parent transaction_id IS the branch name.
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
    auto refreshed_it = transactions_.find(transaction_id);
    if (refreshed_it == transactions_.end()) {
      return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
    }
  }

  for (uint32_t attempts_performed = 1; attempts_performed <= options_.conflict_options.max_attempts; ++attempts_performed) {
    auto merge_or = storage_->Merge(transaction_id, target_branch);
    if (!merge_or.ok()) {
      return merge_or.status();
    }

    const MergeResult& merge_result = *merge_or;
    if (merge_result.IsConflict()) {
      const RetryDecision retry_decision =
          EvaluateRetryDecision(merge_result.GetConflict(), attempts_performed, options_.conflict_options, options_.path_conflict_classifier);
      artifact_system::CommitConflict detail =
          BuildCommitConflict(merge_result.GetConflict(), attempts_performed, options_.conflict_options, options_.path_conflict_classifier);

      if (!retry_decision.retryable) {
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
          return resolved_or.status();
        }
        if (!*resolved_or) {
          detail.set_retryable(false);
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

      auto refreshed_it = transactions_.find(transaction_id);
      if (refreshed_it == transactions_.end()) {
        return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
      }
      if (!refreshed_it->second.child_transaction_ids.empty()) {
        return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
      }
      continue;
    }

    auto remove_status = RemoveTransactionLocked(transaction_id);
    if (!remove_status.ok()) {
      return remove_status;
    }
    auto delete_status = storage_->DeleteBranch(transaction_id);
    if (!delete_status.ok()) {
      options_.on_cleanup_failure(delete_status);
    }

    // The snapshot_id IS the merge commit hash.
    const std::string& new_snapshot_id = merge_result.GetSuccess().commit_id;
    snapshots_[new_snapshot_id] = SnapshotSource{
        .source_transaction_id = transaction_id,
    };

    return CommitSuccess{
        .transaction_id = transaction_id,
        .commit_id = merge_result.GetSuccess().commit_id,
        .snapshot_id = new_snapshot_id,
    };
  }

  return absl::InternalError("commit loop exited unexpectedly");
}

absl::Status TransactionManager::RollbackTransaction(const std::string& transaction_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
  }
  if (!transaction_it->second.child_transaction_ids.empty()) {
    return absl::InvalidArgumentError(absl::StrCat("transaction has active nested children: ", transaction_id));
  }

  // The transaction_id IS the branch name.
  auto delete_status = storage_->DeleteBranch(transaction_id);
  if (!delete_status.ok()) {
    return delete_status;
  }

  return RemoveTransactionLocked(transaction_id);
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

  const auto snapshot_it = snapshots_.find(snapshot_id);
  if (snapshot_it == snapshots_.end()) {
    return absl::NotFoundError(absl::StrCat("snapshot not found: ", snapshot_id));
  }

  return SnapshotMetadata{
      .snapshot_id = snapshot_id,
      .source_transaction_id = snapshot_it->second.source_transaction_id,
  };
}

absl::StatusOr<TransactionManager::TransactionMetadata> TransactionManager::GetTransactionMetadata(const std::string& transaction_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
  }

  return TransactionMetadata{
      .transaction_id = transaction_id,
      .parent_transaction_id = transaction_it->second.parent_transaction_id,
      .parent_snapshot_id = transaction_it->second.parent_snapshot_id,
      .depth = transaction_it->second.depth,
  };
}

} // namespace artifact_system::transaction

#include "transaction/transaction_manager.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "index/index_conflict_resolver.h"
#include "transaction/conflict_resolver.h"

namespace artifact_system::transaction {
namespace {

std::string TransactionBranchName(uint64_t transaction_id) {
  return absl::StrCat("txn-", transaction_id);
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

uint64_t TransactionManager::NextIdLocked() {
  return next_id_++;
}

absl::StatusOr<uint64_t> TransactionManager::CreateSnapshot(std::optional<uint64_t> parent_transaction_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  std::string commit_id;
  std::optional<uint64_t> source_transaction_id;

  if (parent_transaction_id.has_value()) {
    const uint64_t parent_id = *parent_transaction_id;
    const auto transaction_it = transactions_.find(parent_id);
    if (transaction_it == transactions_.end()) {
      if (snapshots_.contains(parent_id)) {
        return absl::InvalidArgumentError(absl::StrCat("parent id references snapshot; expected transaction id: ", parent_id));
      }
      return absl::NotFoundError(absl::StrCat("transaction not found: ", parent_id));
    }

    auto branch_head_or = storage_->GetBranchHead(transaction_it->second.branch_name);
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

  const uint64_t snapshot_id = NextIdLocked();
  snapshots_[snapshot_id] = SnapshotRecord{
      .commit_id = std::move(commit_id),
      .source_transaction_id = source_transaction_id,
  };
  return snapshot_id;
}

absl::StatusOr<uint64_t> TransactionManager::CreateTransaction(std::optional<uint64_t> parent_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }

  std::optional<uint64_t> parent_transaction_id;
  std::optional<uint64_t> parent_snapshot_id;
  uint32_t depth = 0;
  std::string base_commit_id;

  if (parent_id.has_value()) {
    const uint64_t id = *parent_id;
    const auto transaction_it = transactions_.find(id);
    if (transaction_it != transactions_.end()) {
      parent_transaction_id = id;
      depth = transaction_it->second.depth + 1;

      auto branch_head_or = storage_->GetBranchHead(transaction_it->second.branch_name);
      if (!branch_head_or.ok()) {
        return branch_head_or.status();
      }
      base_commit_id = *branch_head_or;
    } else {
      const auto snapshot_it = snapshots_.find(id);
      if (snapshot_it == snapshots_.end()) {
        return absl::NotFoundError(absl::StrCat("parent id not found: ", id));
      }
      parent_snapshot_id = id;
      base_commit_id = snapshot_it->second.commit_id;
    }
  } else {
    auto branch_head_or = storage_->GetBranchHead(storage_->GetCanonicalBranch());
    if (!branch_head_or.ok()) {
      return branch_head_or.status();
    }
    base_commit_id = *branch_head_or;
  }

  const uint64_t transaction_id = NextIdLocked();
  const std::string branch_name = TransactionBranchName(transaction_id);

  auto branch_or = storage_->CreateBranch(branch_name, base_commit_id);
  if (!branch_or.ok()) {
    return branch_or.status();
  }

  transactions_[transaction_id] = TransactionRecord{
      .branch_name = branch_name,
      .parent_transaction_id = parent_transaction_id,
      .parent_snapshot_id = parent_snapshot_id,
      .depth = depth,
      .child_transaction_ids = {},
  };

  if (parent_transaction_id.has_value()) {
    transactions_.at(*parent_transaction_id).child_transaction_ids.insert(transaction_id);
  }

  return transaction_id;
}

absl::Status TransactionManager::RemoveTransactionLocked(uint64_t transaction_id) {
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

absl::StatusOr<TransactionManager::CommitResult> TransactionManager::CommitTransaction(uint64_t transaction_id) {
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

  auto commit_or = storage_->Commit(transaction.branch_name, absl::StrCat("transaction commit ", transaction_id));
  if (!commit_or.ok()) {
    return commit_or.status();
  }

  std::string target_branch;
  if (transaction.parent_transaction_id.has_value()) {
    auto parent_it = transactions_.find(*transaction.parent_transaction_id);
    if (parent_it == transactions_.end()) {
      return absl::NotFoundError(absl::StrCat("parent transaction not found: ", *transaction.parent_transaction_id));
    }
    target_branch = parent_it->second.branch_name;
  } else {
    target_branch = storage_->GetCanonicalBranch();
  }

  for (uint32_t attempts_performed = 1; attempts_performed <= options_.conflict_options.max_attempts; ++attempts_performed) {
    auto merge_or = storage_->Merge(transaction.branch_name, target_branch);
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
        resolution_context.source_branch = transaction.branch_name;
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

    const std::string transaction_branch = transaction.branch_name;

    auto remove_status = RemoveTransactionLocked(transaction_id);
    if (!remove_status.ok()) {
      return remove_status;
    }
    auto delete_status = storage_->DeleteBranch(transaction_branch);
    if (!delete_status.ok()) {
      options_.on_cleanup_failure(delete_status);
    }

    const uint64_t new_snapshot_id = NextIdLocked();
    snapshots_[new_snapshot_id] = SnapshotRecord{
        .commit_id = merge_result.GetSuccess().commit_id,
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

absl::Status TransactionManager::RollbackTransaction(uint64_t transaction_id) {
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

  auto delete_status = storage_->DeleteBranch(transaction_it->second.branch_name);
  if (!delete_status.ok()) {
    return delete_status;
  }

  return RemoveTransactionLocked(transaction_id);
}

absl::StatusOr<TransactionManager::CommitResult> TransactionManager::RunImplicitTransaction(const ImplicitTransactionCallback& callback,
                                                                                            std::optional<uint64_t> parent_id) {
  if (callback == nullptr) {
    return absl::InvalidArgumentError("callback is null");
  }

  auto transaction_id_or = CreateTransaction(parent_id);
  if (!transaction_id_or.ok()) {
    return transaction_id_or.status();
  }
  const uint64_t transaction_id = *transaction_id_or;

  const absl::Status callback_status = callback(transaction_id);
  if (!callback_status.ok()) {
    const absl::Status rollback_status = RollbackTransaction(transaction_id);
    if (!rollback_status.ok()) {
      return absl::InternalError(absl::StrCat("callback failed and rollback failed: ", rollback_status.message()));
    }
    return callback_status;
  }

  auto commit_or = CommitTransaction(transaction_id);
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

absl::StatusOr<TransactionManager::SnapshotMetadata> TransactionManager::GetSnapshotMetadata(uint64_t snapshot_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto snapshot_it = snapshots_.find(snapshot_id);
  if (snapshot_it == snapshots_.end()) {
    return absl::NotFoundError(absl::StrCat("snapshot not found: ", snapshot_id));
  }

  return SnapshotMetadata{
      .snapshot_id = snapshot_id,
      .commit_id = snapshot_it->second.commit_id,
      .source_transaction_id = snapshot_it->second.source_transaction_id,
  };
}

absl::StatusOr<TransactionManager::TransactionMetadata> TransactionManager::GetTransactionMetadata(uint64_t transaction_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto transaction_it = transactions_.find(transaction_id);
  if (transaction_it == transactions_.end()) {
    return absl::NotFoundError(absl::StrCat("transaction not found: ", transaction_id));
  }

  return TransactionMetadata{
      .transaction_id = transaction_id,
      .branch_name = transaction_it->second.branch_name,
      .parent_transaction_id = transaction_it->second.parent_transaction_id,
      .parent_snapshot_id = transaction_it->second.parent_snapshot_id,
      .depth = transaction_it->second.depth,
  };
}

} // namespace artifact_system::transaction

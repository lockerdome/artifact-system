#include "transaction/write_executor.h"

#include <cstdint>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "index/index_conflict_resolver.h"
#include "util/uuid.h"

namespace artifact_system::transaction {

WriteExecutor::WriteExecutor(StorageInterface* storage, WriteExecutorOptions options) : storage_(storage), options_(std::move(options)) {
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

std::string WriteExecutor::NextChildBranchName(const std::string& transaction_branch) {
  return absl::StrCat(transaction_branch, ".write-", util::GenerateUUID());
}

absl::StatusOr<WriteExecutor::WriteResult> WriteExecutor::ExecuteWrite(const std::string& transaction_branch, const StagingCallback& staging_callback) {
  if (storage_ == nullptr) {
    return absl::FailedPreconditionError("storage is null");
  }
  if (staging_callback == nullptr) {
    return absl::InvalidArgumentError("staging callback is null");
  }
  if (options_.conflict_options.max_attempts == 0) {
    return absl::InvalidArgumentError("max attempts must be greater than zero");
  }

  auto branch_head_or = storage_->GetBranchHead(transaction_branch);
  if (!branch_head_or.ok()) {
    return branch_head_or.status();
  }

  const std::string child_branch = NextChildBranchName(transaction_branch);
  auto child_branch_or = storage_->CreateBranch(child_branch, *branch_head_or);
  if (!child_branch_or.ok()) {
    return child_branch_or.status();
  }

  auto cleanup_child_branch = [&]() {
    const absl::Status cleanup_status = storage_->DeleteBranch(child_branch);
    if (!cleanup_status.ok()) {
      options_.on_cleanup_failure(cleanup_status);
    }
  };

  const absl::Status staging_status = staging_callback(child_branch);
  if (!staging_status.ok()) {
    cleanup_child_branch();
    return staging_status;
  }

  auto child_commit_or = storage_->Commit(child_branch, "write attempt 1");
  if (!child_commit_or.ok()) {
    cleanup_child_branch();
    return child_commit_or.status();
  }

  for (uint32_t attempts_performed = 1; attempts_performed <= options_.conflict_options.max_attempts; ++attempts_performed) {
    auto merge_or = storage_->Merge(child_branch, transaction_branch);
    if (!merge_or.ok()) {
      cleanup_child_branch();
      return merge_or.status();
    }

    if (merge_or->IsSuccess()) {
      const std::string commit_id = merge_or->GetSuccess().commit_id;
      cleanup_child_branch();
      return WriteSuccess{
          .commit_id = commit_id,
          .attempts = attempts_performed,
      };
    }

    const MergeResult::Conflict& conflict = merge_or->GetConflict();
    const RetryDecision retry_decision = EvaluateRetryDecision(conflict, attempts_performed, options_.conflict_options, options_.path_conflict_classifier);
    CommitConflict conflict_detail = BuildCommitConflict(conflict, attempts_performed, options_.conflict_options, options_.path_conflict_classifier);

    if (!retry_decision.retryable) {
      cleanup_child_branch();
      return WriteConflict{
          .conflict = conflict,
          .detail = conflict_detail,
          .attempts = attempts_performed,
      };
    }

    if (options_.retry_conflict_resolver != nullptr) {
      RetryResolutionContext resolution_context;
      resolution_context.merge_conflict = conflict;
      resolution_context.source_branch = child_branch;
      resolution_context.target_branch = transaction_branch;
      resolution_context.attempts_performed = attempts_performed;
      auto resolved_or = options_.retry_conflict_resolver(resolution_context);
      if (!resolved_or.ok()) {
        cleanup_child_branch();
        return resolved_or.status();
      }
      if (!*resolved_or) {
        conflict_detail.set_retryable(false);
        cleanup_child_branch();
        return WriteConflict{
            .conflict = conflict,
            .detail = conflict_detail,
            .attempts = attempts_performed,
        };
      }
    }

    options_.sleep_for(ComputeBackoffWithJitter(attempts_performed - 1, options_.conflict_options));
  }

  return absl::InternalError("write loop exited unexpectedly");
}

} // namespace artifact_system::transaction

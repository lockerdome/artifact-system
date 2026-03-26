#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "absl/time/time.h"
#include "artifact_service.pb.h"
#include "storage/storage_interface.h"

namespace artifact_system::transaction {

enum class PathConflictKind {
  kRetryableNonUniqueIndex,
  kNonRetryableUniqueIndex,
  kNonRetryablePayload,
  kNonRetryableReferentialIntegrity,
  kNonRetryableUnknown,
};

using PathConflictClassifier = std::function<PathConflictKind(const std::string&)>;

struct ConflictResolverOptions {
  uint32_t max_attempts = 5;
  absl::Duration initial_backoff = absl::Milliseconds(100);
  absl::Duration max_backoff = absl::Seconds(2);
};

struct RetryDecision {
  bool retryable = false;
  CommitConflict::ConflictType conflict_type = CommitConflict::CONFLICT_TYPE_UNSPECIFIED;
};

struct RetryResolutionContext {
  MergeResult::Conflict merge_conflict;
  std::string source_branch;
  std::string target_branch;
  uint32_t attempts_performed = 0;
};

using RetryConflictResolver = std::function<absl::StatusOr<bool>(const RetryResolutionContext&)>;

PathConflictKind DefaultPathConflictClassifier(const std::string& path);

RetryDecision EvaluateRetryDecision(const MergeResult::Conflict& merge_conflict, uint32_t attempts_performed, const ConflictResolverOptions& options,
                                    const PathConflictClassifier& classifier);

absl::Duration ComputeBackoffWithJitter(uint32_t attempt_index, const ConflictResolverOptions& options);

CommitConflict BuildCommitConflict(const MergeResult::Conflict& merge_conflict, uint32_t attempts_performed, const ConflictResolverOptions& options,
                                   const PathConflictClassifier& classifier);

} // namespace artifact_system::transaction

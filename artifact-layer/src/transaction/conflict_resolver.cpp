#include "transaction/conflict_resolver.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "encoding/base64url.h"

namespace artifact_system::transaction {
namespace {

CommitConflict::ConflictType ResolveConflictType(const std::vector<PathConflictKind>& kinds) {
  bool saw_index_conflict = false;
  bool saw_referential_conflict = false;
  bool saw_unknown_conflict = false;

  for (PathConflictKind kind : kinds) {
    switch (kind) {
    case PathConflictKind::kNonRetryablePayload:
      return CommitConflict::PAYLOAD_CONFLICT;
    case PathConflictKind::kRetryableNonUniqueIndex:
    case PathConflictKind::kNonRetryableUniqueIndex:
      saw_index_conflict = true;
      break;
    case PathConflictKind::kNonRetryableReferentialIntegrity:
      saw_referential_conflict = true;
      break;
    case PathConflictKind::kNonRetryableUnknown:
      saw_unknown_conflict = true;
      break;
    }
  }

  if (saw_index_conflict) {
    return CommitConflict::INDEX_CONFLICT;
  }
  if (saw_referential_conflict) {
    return CommitConflict::REFERENTIAL_INTEGRITY_VIOLATION;
  }
  if (saw_unknown_conflict) {
    return CommitConflict::CONFLICT_TYPE_UNSPECIFIED;
  }
  return CommitConflict::CONFLICT_TYPE_UNSPECIFIED;
}

uint64_t StableAttemptHash(uint32_t attempt_index) {
  uint64_t value = static_cast<uint64_t>(attempt_index) + 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::optional<uint64_t> ParseDecimalSuffix(std::string_view suffix) {
  if (suffix.empty()) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (char c : suffix) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  return value;
}

std::optional<uint64_t> DecodeArtifactIdFromPath(const std::string& path) {
  if (path.rfind("payload/", 0) == 0) {
    const size_t slash_pos = path.find_last_of('/');
    if (slash_pos == std::string::npos || slash_pos + 1 >= path.size()) {
      return std::nullopt;
    }
    return ParseDecimalSuffix(std::string_view(path).substr(slash_pos + 1));
  }

  if (path.rfind("artifacts/", 0) != 0) {
    return std::nullopt;
  }
  const std::string_view encoded_id = std::string_view(path).substr(10);
  auto decoded_or = encoding::base64url::Decode(encoded_id);
  if (!decoded_or.ok() || decoded_or->size() != sizeof(uint64_t)) {
    return std::nullopt;
  }

  uint64_t artifact_id = 0;
  for (uint8_t byte : *decoded_or) {
    artifact_id = (artifact_id << 8U) | static_cast<uint64_t>(byte);
  }
  return artifact_id;
}

} // namespace

PathConflictKind DefaultPathConflictClassifier(const std::string& path) {
  if (path.rfind("idx/non_unique/", 0) == 0) {
    return PathConflictKind::kRetryableNonUniqueIndex;
  }
  if (path.rfind("idx/unique/", 0) == 0) {
    return PathConflictKind::kNonRetryableUniqueIndex;
  }
  if (path.rfind("indexes/", 0) == 0) {
    // Real storage index paths do not encode uniqueness in the path itself.
    // Default to non-retryable index conflict unless a concrete classifier is injected.
    return PathConflictKind::kNonRetryableUniqueIndex;
  }
  if (path.rfind("payload/", 0) == 0 || path.rfind("artifacts/", 0) == 0) {
    return PathConflictKind::kNonRetryablePayload;
  }
  if (path.rfind("references/", 0) == 0) {
    return PathConflictKind::kNonRetryableReferentialIntegrity;
  }
  return PathConflictKind::kNonRetryableUnknown;
}

RetryDecision EvaluateRetryDecision(const MergeResult::Conflict& merge_conflict, uint32_t attempts_performed, const ConflictResolverOptions& options,
                                    const PathConflictClassifier& classifier) {
  if (merge_conflict.conflicting_paths.empty()) {
    return {
        .retryable = false,
        .conflict_type = CommitConflict::CONFLICT_TYPE_UNSPECIFIED,
    };
  }

  std::vector<PathConflictKind> kinds;
  kinds.reserve(merge_conflict.conflicting_paths.size());

  bool all_retryable_non_unique = true;
  for (const std::string& path : merge_conflict.conflicting_paths) {
    const PathConflictKind kind = classifier != nullptr ? classifier(path) : DefaultPathConflictClassifier(path);
    kinds.push_back(kind);
    if (kind != PathConflictKind::kRetryableNonUniqueIndex) {
      all_retryable_non_unique = false;
    }
  }

  const bool attempts_exhausted = attempts_performed >= options.max_attempts;

  return {
      .retryable = all_retryable_non_unique && !attempts_exhausted,
      .conflict_type = ResolveConflictType(kinds),
  };
}

absl::Duration ComputeBackoffWithJitter(uint32_t attempt_index, const ConflictResolverOptions& options) {
  if (options.max_backoff <= absl::ZeroDuration()) {
    return absl::ZeroDuration();
  }

  absl::Duration backoff = std::max(options.initial_backoff, absl::ZeroDuration());
  for (uint32_t i = 0; i < attempt_index && backoff < options.max_backoff; ++i) {
    backoff *= 2;
    if (backoff > options.max_backoff) {
      backoff = options.max_backoff;
    }
  }

  if (backoff >= options.max_backoff) {
    return options.max_backoff;
  }

  const absl::Duration max_jitter = std::min(options.max_backoff - backoff, backoff / 2);
  if (max_jitter <= absl::ZeroDuration()) {
    return backoff;
  }

  const double unit = static_cast<double>(StableAttemptHash(attempt_index) & 0xFFFFULL) / 65535.0;
  return backoff + max_jitter * unit;
}

CommitConflict BuildCommitConflict(const MergeResult::Conflict& merge_conflict, uint32_t attempts_performed, const ConflictResolverOptions& options,
                                   const PathConflictClassifier& classifier) {
  const RetryDecision decision = EvaluateRetryDecision(merge_conflict, attempts_performed, options, classifier);

  CommitConflict conflict;
  conflict.set_conflict_type(decision.conflict_type);
  conflict.set_retryable(decision.retryable);
  conflict.set_attempts(attempts_performed);

  if (decision.conflict_type == CommitConflict::PAYLOAD_CONFLICT) {
    auto* detail = conflict.mutable_payload_detail();
    for (const std::string& path : merge_conflict.conflicting_paths) {
      const PathConflictKind kind = classifier != nullptr ? classifier(path) : DefaultPathConflictClassifier(path);
      if (kind == PathConflictKind::kNonRetryablePayload) {
        if (auto artifact_id = DecodeArtifactIdFromPath(path); artifact_id.has_value()) {
          detail->set_artifact_id(*artifact_id);
        }
        break;
      }
    }
  }
  if (decision.conflict_type == CommitConflict::INDEX_CONFLICT) {
    auto* detail = conflict.mutable_index_detail();
    if (!merge_conflict.conflicting_paths.empty()) {
      const std::string& path = merge_conflict.conflicting_paths.front();
      detail->set_key_type(path);
    }
  }
  if (decision.conflict_type == CommitConflict::REFERENTIAL_INTEGRITY_VIOLATION) {
    conflict.mutable_referential_integrity_detail();
  }

  if (!merge_conflict.base_commit_id.empty()) {
    conflict.set_base_commit_id(merge_conflict.base_commit_id);
  }
  if (!merge_conflict.source_commit_id.empty()) {
    conflict.set_ours_commit_id(merge_conflict.source_commit_id);
  }
  if (!merge_conflict.target_commit_id.empty()) {
    conflict.set_theirs_commit_id(merge_conflict.target_commit_id);
  }

  return conflict;
}

} // namespace artifact_system::transaction

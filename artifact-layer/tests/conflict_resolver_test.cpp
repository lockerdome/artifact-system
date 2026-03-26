#include "transaction/conflict_resolver.h"

#include <cstdint>
#include <string>

#include "absl/time/time.h"
#include "encoding/artifact_path.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::transaction::ConflictResolverOptions;
using artifact_system::transaction::PathConflictKind;

MergeResult::Conflict BuildConflict(std::initializer_list<std::string> paths) {
  MergeResult::Conflict conflict;
  for (const std::string& path : paths) {
    conflict.conflicting_paths.push_back(path);
  }
  conflict.base_commit_id = "base-1";
  conflict.source_commit_id = "source-1";
  conflict.target_commit_id = "target-1";
  return conflict;
}

PathConflictKind ClassifyPath(const std::string& path) {
  if (path.rfind("idx/non_unique/", 0) == 0) {
    return PathConflictKind::kRetryableNonUniqueIndex;
  }
  if (path.rfind("idx/unique/", 0) == 0) {
    return PathConflictKind::kNonRetryableUniqueIndex;
  }
  if (path.rfind("payload/", 0) == 0) {
    return PathConflictKind::kNonRetryablePayload;
  }
  return PathConflictKind::kNonRetryableUnknown;
}

TEST(ConflictResolverTest, AllRetryablePathsAreEligibleForRetry) {
  const MergeResult::Conflict conflict = BuildConflict({"idx/non_unique/a", "idx/non_unique/b"});
  const ConflictResolverOptions options;

  const auto decision = transaction::EvaluateRetryDecision(conflict, 1, options, ClassifyPath);
  EXPECT_TRUE(decision.retryable);
  EXPECT_EQ(decision.conflict_type, CommitConflict::INDEX_CONFLICT);
}

TEST(ConflictResolverTest, MixedRetryableAndPayloadConflictIsNotEligible) {
  const MergeResult::Conflict conflict = BuildConflict({"idx/non_unique/a", "payload/artifact/42"});
  const ConflictResolverOptions options;

  const auto decision = transaction::EvaluateRetryDecision(conflict, 1, options, ClassifyPath);
  EXPECT_FALSE(decision.retryable);
  EXPECT_EQ(decision.conflict_type, CommitConflict::PAYLOAD_CONFLICT);

  const CommitConflict proto_conflict = transaction::BuildCommitConflict(conflict, 1, options, ClassifyPath);
  EXPECT_EQ(proto_conflict.conflict_type(), CommitConflict::PAYLOAD_CONFLICT);
  EXPECT_EQ(proto_conflict.detail_case(), CommitConflict::kPayloadDetail);
}

TEST(ConflictResolverTest, EmptyConflictPathsAreNonRetryable) {
  const MergeResult::Conflict conflict = BuildConflict({});
  const ConflictResolverOptions options;

  const auto decision = transaction::EvaluateRetryDecision(conflict, 1, options, ClassifyPath);
  EXPECT_FALSE(decision.retryable);
}

TEST(ConflictResolverTest, ExhaustionSetsRetryableFalseInCommitConflict) {
  const MergeResult::Conflict conflict = BuildConflict({"idx/non_unique/a"});
  ConflictResolverOptions options;
  options.max_attempts = 2;

  const CommitConflict proto_conflict = transaction::BuildCommitConflict(conflict, 2, options, ClassifyPath);
  EXPECT_FALSE(proto_conflict.retryable());
  EXPECT_EQ(proto_conflict.attempts(), 2);
  EXPECT_TRUE(proto_conflict.has_base_commit_id());
  EXPECT_EQ(proto_conflict.base_commit_id(), "base-1");
  EXPECT_TRUE(proto_conflict.has_ours_commit_id());
  EXPECT_EQ(proto_conflict.ours_commit_id(), "source-1");
  EXPECT_TRUE(proto_conflict.has_theirs_commit_id());
  EXPECT_EQ(proto_conflict.theirs_commit_id(), "target-1");
}

TEST(ConflictResolverTest, BackoffIsBoundedAndNonDecreasing) {
  ConflictResolverOptions options;
  options.initial_backoff = absl::Milliseconds(10);
  options.max_backoff = absl::Milliseconds(120);

  absl::Duration previous = absl::ZeroDuration();
  for (uint32_t attempt = 0; attempt < 10; ++attempt) {
    const absl::Duration current = transaction::ComputeBackoffWithJitter(attempt, options);
    EXPECT_GE(current, previous);
    EXPECT_LE(current, options.max_backoff);
    previous = current;
  }
}

TEST(ConflictResolverTest, UsesDefaultClassifierWhenCallbackNotProvided) {
  const MergeResult::Conflict conflict = BuildConflict({"indexes/prefix/hash"});
  const ConflictResolverOptions options;

  const auto decision = transaction::EvaluateRetryDecision(conflict, 1, options, nullptr);
  EXPECT_FALSE(decision.retryable);
  EXPECT_EQ(decision.conflict_type, CommitConflict::INDEX_CONFLICT);
}

TEST(ConflictResolverTest, ReferencialConflictTypeCanBeProduced) {
  const MergeResult::Conflict conflict = BuildConflict({"references/target/1"});
  const ConflictResolverOptions options;

  const auto decision = transaction::EvaluateRetryDecision(conflict, 1, options, nullptr);
  EXPECT_FALSE(decision.retryable);
  EXPECT_EQ(decision.conflict_type, CommitConflict::REFERENTIAL_INTEGRITY_VIOLATION);
}

TEST(ConflictResolverTest, PayloadConflictDecodesArtifactPathId) {
  const std::string path = encoding::ArtifactPath(42);
  const MergeResult::Conflict conflict = BuildConflict({path});
  const ConflictResolverOptions options;

  const CommitConflict proto_conflict = transaction::BuildCommitConflict(conflict, 1, options, nullptr);
  EXPECT_EQ(proto_conflict.conflict_type(), CommitConflict::PAYLOAD_CONFLICT);
  EXPECT_TRUE(proto_conflict.has_payload_detail());
  EXPECT_EQ(proto_conflict.payload_detail().artifact_id(), 42);
}

} // namespace
} // namespace artifact_system::testing

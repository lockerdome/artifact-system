#include "transaction/transaction_manager.h"

#include <optional>
#include <string>
#include <variant>

#include "absl/status/status.h"
#include "absl/time/time.h"
#include "storage/memory_storage.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::transaction::PathConflictKind;
using artifact_system::transaction::TransactionManager;

TEST(TransactionManagerTest, CreateSnapshotFromCanonicalAndTransaction) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto canonical_snapshot_id_or = manager.CreateSnapshot();
  ASSERT_TRUE(canonical_snapshot_id_or.ok());

  auto canonical_snapshot_meta_or = manager.GetSnapshotMetadata(*canonical_snapshot_id_or);
  ASSERT_TRUE(canonical_snapshot_meta_or.ok());

  auto canonical_head_or = storage.GetBranchHead(storage.GetCanonicalBranch());
  ASSERT_TRUE(canonical_head_or.ok());
  EXPECT_EQ(canonical_snapshot_meta_or->commit_id, *canonical_head_or);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());
  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, "a.txt", "value-a").ok());
  auto tx_commit_or = storage.Commit(transaction_meta_or->branch_name, "tx work");
  ASSERT_TRUE(tx_commit_or.ok());

  auto tx_snapshot_id_or = manager.CreateSnapshot(*transaction_id_or);
  ASSERT_TRUE(tx_snapshot_id_or.ok());

  auto tx_snapshot_meta_or = manager.GetSnapshotMetadata(*tx_snapshot_id_or);
  ASSERT_TRUE(tx_snapshot_meta_or.ok());
  EXPECT_EQ(tx_snapshot_meta_or->commit_id, *tx_commit_or);
}

TEST(TransactionManagerTest, TransactionLifecycleCommitAndRollback) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto commit_tx_id_or = manager.CreateTransaction();
  ASSERT_TRUE(commit_tx_id_or.ok());

  auto commit_result_or = manager.CommitTransaction(*commit_tx_id_or);
  ASSERT_TRUE(commit_result_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_result_or));

  const auto& committed = std::get<TransactionManager::CommitSuccess>(*commit_result_or);
  ASSERT_TRUE(committed.snapshot_id.has_value());

  auto snapshot_meta_or = manager.GetSnapshotMetadata(*committed.snapshot_id);
  ASSERT_TRUE(snapshot_meta_or.ok());
  auto canonical_head_or = storage.GetBranchHead(storage.GetCanonicalBranch());
  ASSERT_TRUE(canonical_head_or.ok());
  EXPECT_EQ(snapshot_meta_or->commit_id, *canonical_head_or);

  auto missing_after_commit = manager.GetTransactionMetadata(*commit_tx_id_or);
  ASSERT_FALSE(missing_after_commit.ok());
  EXPECT_EQ(missing_after_commit.status().code(), absl::StatusCode::kNotFound);

  auto rollback_tx_id_or = manager.CreateTransaction();
  ASSERT_TRUE(rollback_tx_id_or.ok());
  ASSERT_TRUE(manager.RollbackTransaction(*rollback_tx_id_or).ok());

  auto missing_after_rollback = manager.GetTransactionMetadata(*rollback_tx_id_or);
  ASSERT_FALSE(missing_after_rollback.ok());
  EXPECT_EQ(missing_after_rollback.status().code(), absl::StatusCode::kNotFound);
}

TEST(TransactionManagerTest, NestedCommitMergesIntoParentThenCanonicalOnParentCommit) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto parent_tx_id_or = manager.CreateTransaction();
  ASSERT_TRUE(parent_tx_id_or.ok());
  auto parent_meta_or = manager.GetTransactionMetadata(*parent_tx_id_or);
  ASSERT_TRUE(parent_meta_or.ok());

  auto child_tx_id_or = manager.CreateTransaction(*parent_tx_id_or);
  ASSERT_TRUE(child_tx_id_or.ok());
  auto child_meta_or = manager.GetTransactionMetadata(*child_tx_id_or);
  ASSERT_TRUE(child_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(child_meta_or->branch_name, "nested/object.txt", "nested-data").ok());

  auto child_commit_or = manager.CommitTransaction(*child_tx_id_or);
  ASSERT_TRUE(child_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*child_commit_or));
  EXPECT_TRUE(std::get<TransactionManager::CommitSuccess>(*child_commit_or).snapshot_id.has_value());

  auto parent_read_or = storage.GetObject(parent_meta_or->branch_name, "nested/object.txt");
  ASSERT_TRUE(parent_read_or.ok());
  EXPECT_EQ(*parent_read_or, "nested-data");

  auto canonical_before_parent_commit_or = storage.GetObject(storage.GetCanonicalBranch(), "nested/object.txt");
  ASSERT_FALSE(canonical_before_parent_commit_or.ok());
  EXPECT_EQ(canonical_before_parent_commit_or.status().code(), absl::StatusCode::kNotFound);

  auto parent_commit_or = manager.CommitTransaction(*parent_tx_id_or);
  ASSERT_TRUE(parent_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*parent_commit_or));

  auto canonical_after_parent_commit_or = storage.GetObject(storage.GetCanonicalBranch(), "nested/object.txt");
  ASSERT_TRUE(canonical_after_parent_commit_or.ok());
  EXPECT_EQ(*canonical_after_parent_commit_or, "nested-data");
}

TEST(TransactionManagerTest, ImplicitTransactionSuccessCommitsAndReturnsSnapshotId) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto implicit_or = manager.RunImplicitTransaction([&](uint64_t tx_id) {
    auto tx_meta_or = manager.GetTransactionMetadata(tx_id);
    if (!tx_meta_or.ok()) {
      return tx_meta_or.status();
    }
    return storage.PutObject(tx_meta_or->branch_name, "implicit/success.txt", "ok");
  });

  ASSERT_TRUE(implicit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*implicit_or));
  const auto& committed = std::get<TransactionManager::CommitSuccess>(*implicit_or);
  ASSERT_TRUE(committed.snapshot_id.has_value());

  auto canonical_read_or = storage.GetObject(storage.GetCanonicalBranch(), "implicit/success.txt");
  ASSERT_TRUE(canonical_read_or.ok());
  EXPECT_EQ(*canonical_read_or, "ok");
}

TEST(TransactionManagerTest, ImplicitTransactionFailureRollsBack) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  std::optional<uint64_t> seen_tx_id;
  auto implicit_or = manager.RunImplicitTransaction([&](uint64_t tx_id) {
    seen_tx_id = tx_id;
    auto tx_meta_or = manager.GetTransactionMetadata(tx_id);
    if (!tx_meta_or.ok()) {
      return tx_meta_or.status();
    }
    auto put_status = storage.PutObject(tx_meta_or->branch_name, "implicit/failure.txt", "temp");
    if (!put_status.ok()) {
      return put_status;
    }
    return absl::InvalidArgumentError("callback failure");
  });

  ASSERT_FALSE(implicit_or.ok());
  EXPECT_EQ(implicit_or.status().code(), absl::StatusCode::kInvalidArgument);
  ASSERT_TRUE(seen_tx_id.has_value());

  auto tx_meta_or = manager.GetTransactionMetadata(*seen_tx_id);
  ASSERT_FALSE(tx_meta_or.ok());
  EXPECT_EQ(tx_meta_or.status().code(), absl::StatusCode::kNotFound);

  auto canonical_read_or = storage.GetObject(storage.GetCanonicalBranch(), "implicit/failure.txt");
  ASSERT_FALSE(canonical_read_or.ok());
  EXPECT_EQ(canonical_read_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(TransactionManagerTest, CommitTransactionClassifiesNonRetryableIndexConflict) {
  MemoryStorage storage;

  TransactionManager::Options options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/unique/", 0) == 0) {
      return PathConflictKind::kNonRetryableUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, "idx/unique/key", "tx-value").ok());

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), "idx/unique/key", "canonical-value").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));

  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_EQ(conflict.detail.conflict_type(), CommitConflict::INDEX_CONFLICT);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.attempts(), 1);
  EXPECT_EQ(conflict.detail.index_detail().key_type(), "idx/unique/key");
}

TEST(TransactionManagerTest, CommitTransactionRetriesUntilExhaustedForRetryableConflicts) {
  MemoryStorage storage;

  TransactionManager::Options options;
  options.conflict_options.max_attempts = 3;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, "idx/non_unique/key", "tx-value").ok());

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), "idx/non_unique/key", "canonical-value").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));

  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.attempts(), 3);
  EXPECT_EQ(conflict.detail.conflict_type(), CommitConflict::INDEX_CONFLICT);
}

TEST(TransactionManagerTest, ImplicitTransactionRejectsNullCallback) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto result_or = manager.RunImplicitTransaction({});
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(TransactionManagerTest, CommitTransactionInvokesRetryConflictResolver) {
  MemoryStorage storage;

  TransactionManager::Options options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  uint32_t resolver_calls = 0;
  options.retry_conflict_resolver = [&](const artifact_system::transaction::RetryResolutionContext& context) {
    ++resolver_calls;
    EXPECT_EQ(context.target_branch, storage.GetCanonicalBranch());
    EXPECT_EQ(context.attempts_performed, 1);
    return absl::StatusOr<bool>(false);
  };
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, "idx/non_unique/key", "tx-value").ok());

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), "idx/non_unique/key", "canonical-value").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));
  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.attempts(), 1);
  EXPECT_EQ(resolver_calls, 1);
}

} // namespace
} // namespace artifact_system::testing

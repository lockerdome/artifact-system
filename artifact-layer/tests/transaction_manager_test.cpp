#include "transaction/transaction_manager.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "artifact_options.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "storage/memory_storage.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::transaction::PathConflictKind;
using artifact_system::transaction::TransactionManager;

absl::StatusOr<artifact_system::IndexDefinition> FindIndexDefinitionByKeyType(const google::protobuf::Descriptor& descriptor, const std::string& key_type) {
  const auto& options = descriptor.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    const auto& definition = options.GetExtension(artifact_system::indexes, i);
    if (definition.key_type() == key_type) {
      return definition;
    }
  }
  return absl::NotFoundError("index definition not found");
}

// Builds a valid serialized index object for mock expectations. Rows are intentionally
// empty — merge correctness with real row data is covered by index_merge_test.cpp.
absl::StatusOr<std::string> BuildIndexObjectBytes(const artifact_system::IndexDefinition& definition, const google::protobuf::Descriptor& descriptor,
                                                  const std::string& serialized_key, std::initializer_list<uint64_t> /*artifact_ids*/) {
  auto schema_or = artifact_system::index::GenerateIndexSchema(definition, descriptor);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  artifact_system::index::IndexObject object;
  object.serialized_key = serialized_key;

  return artifact_system::index::SerializeIndexObject(*schema_or, definition, object);
}

TEST(TransactionManagerTest, CreateSnapshotFromCanonicalAndTransaction) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto canonical_snapshot_id_or = manager.CreateSnapshot();
  ASSERT_TRUE(canonical_snapshot_id_or.ok());
  EXPECT_FALSE(canonical_snapshot_id_or->empty());

  auto canonical_head_or = storage.GetBranchHead(storage.GetCanonicalBranch());
  ASSERT_TRUE(canonical_head_or.ok());
  // The snapshot_id IS the commit hash.
  EXPECT_EQ(*canonical_snapshot_id_or, *canonical_head_or);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  // The transaction_id IS the branch name — use it directly for storage operations.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, "a.txt", "value-a").ok());
  auto tx_commit_or = storage.Commit(*transaction_id_or, "tx work");
  ASSERT_TRUE(tx_commit_or.ok());

  auto tx_snapshot_id_or = manager.CreateSnapshot(*transaction_id_or);
  ASSERT_TRUE(tx_snapshot_id_or.ok());
  // The snapshot_id IS the commit hash.
  EXPECT_EQ(*tx_snapshot_id_or, *tx_commit_or);
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
  EXPECT_FALSE(committed.snapshot_id.empty());

  auto canonical_head_or = storage.GetBranchHead(storage.GetCanonicalBranch());
  ASSERT_TRUE(canonical_head_or.ok());
  // The snapshot_id IS the commit hash.
  EXPECT_EQ(committed.snapshot_id, *canonical_head_or);

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

  // Create child transaction with parent_transaction_id.
  auto child_tx_id_or = manager.CreateTransaction(/*parent_snapshot_id=*/std::nullopt, /*parent_transaction_id=*/*parent_tx_id_or);
  ASSERT_TRUE(child_tx_id_or.ok());

  // The transaction_id IS the branch name — use directly.
  ASSERT_TRUE(storage.PutObject(*child_tx_id_or, "nested/object.txt", "nested-data").ok());
  ASSERT_TRUE(storage.Commit(*child_tx_id_or, "child write").ok());

  auto child_commit_or = manager.CommitTransaction(*child_tx_id_or);
  ASSERT_TRUE(child_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*child_commit_or));
  EXPECT_FALSE(std::get<TransactionManager::CommitSuccess>(*child_commit_or).snapshot_id.empty());

  // Parent branch should now contain the nested object.
  auto parent_read_or = storage.GetObject(*parent_tx_id_or, "nested/object.txt");
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

  auto implicit_or = manager.RunImplicitTransaction([&](const std::string& tx_id) {
    // The tx_id IS the branch name — use directly.
    return storage.PutObject(tx_id, "implicit/success.txt", "ok");
  });

  ASSERT_TRUE(implicit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*implicit_or));
  const auto& committed = std::get<TransactionManager::CommitSuccess>(*implicit_or);
  EXPECT_FALSE(committed.snapshot_id.empty());

  auto canonical_read_or = storage.GetObject(storage.GetCanonicalBranch(), "implicit/success.txt");
  ASSERT_TRUE(canonical_read_or.ok());
  EXPECT_EQ(*canonical_read_or, "ok");
}

TEST(TransactionManagerTest, ImplicitTransactionFailureRollsBack) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  std::optional<std::string> seen_tx_id;
  auto implicit_or = manager.RunImplicitTransaction([&](const std::string& tx_id) {
    seen_tx_id = tx_id;
    // The tx_id IS the branch name — use directly.
    auto put_status = storage.PutObject(tx_id, "implicit/failure.txt", "temp");
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

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, "idx/unique/key", "tx-value").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());

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

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, "idx/non_unique/key", "tx-value").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());

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

TEST(TransactionManagerTest, DefaultResolverDoesNotAutoResolveUniqueIndexConflict) {
  MemoryStorage storage;

  auto index_definition_or = FindIndexDefinitionByKeyType(*artifact_system::IndexDefinition::descriptor(), "index_key_type_unique");
  ASSERT_TRUE(index_definition_or.ok());
  const artifact_system::IndexDefinition index_definition = *index_definition_or;

  const uint64_t index_definition_id = 7002;
  artifact_system::IndexDefinition index_payload = index_definition;
  const std::string index_definition_path = encoding::ArtifactPath(index_definition_id);

  const std::string index_path = encoding::IndexPath(index_definition_id, {});

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_definition_path, index_payload.SerializeAsString()).ok());
  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_path, "base").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "seed unique index base").ok());

  TransactionManager::Options options;
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, index_path, "ours").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_path, "theirs").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical unique update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));
  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.conflict_type(), CommitConflict::INDEX_CONFLICT);
  EXPECT_EQ(conflict.detail.attempts(), 1);
}

TEST(TransactionManagerTest, DefaultResolverDoesNotAutoResolveNonIndexConflict) {
  MemoryStorage storage;
  TransactionManager::Options options;
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, "payload/object-1", "txn-value").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());
  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), "payload/object-1", "canonical-value").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical payload update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));
  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.conflict_type(), CommitConflict::PAYLOAD_CONFLICT);
  EXPECT_EQ(conflict.detail.attempts(), 1);
}

TEST(TransactionManagerTest, DefaultResolverTreatsMalformedIndexDefinitionAsNonRetryableConflict) {
  MemoryStorage storage;

  const uint64_t index_definition_id = 7003;
  const std::string index_definition_path = encoding::ArtifactPath(index_definition_id);
  const std::string index_path = encoding::IndexPath(index_definition_id, {});

  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_definition_path, "not-a-valid-index-definition").ok());
  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_path, "base").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "seed malformed index definition").ok());

  TransactionManager::Options options;
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, index_path, "txn-value").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());
  ASSERT_TRUE(storage.PutObject(storage.GetCanonicalBranch(), index_path, "canonical-value").ok());
  ASSERT_TRUE(storage.Commit(storage.GetCanonicalBranch(), "canonical malformed index update").ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*commit_or));
  const auto& conflict = std::get<TransactionManager::CommitConflict>(*commit_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.conflict_type(), CommitConflict::CONFLICT_TYPE_UNSPECIFIED);
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

  // The transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*transaction_id_or, "idx/non_unique/key", "tx-value").ok());
  ASSERT_TRUE(storage.Commit(*transaction_id_or, "tx write").ok());

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

TEST(TransactionManagerTest, CommitTransactionRejectsZeroMaxAttempts) {
  MemoryStorage storage;

  TransactionManager::Options options;
  options.conflict_options.max_attempts = 0;
  TransactionManager manager(&storage, options);

  auto transaction_id_or = manager.CreateTransaction();
  ASSERT_TRUE(transaction_id_or.ok());

  auto commit_or = manager.CommitTransaction(*transaction_id_or);
  ASSERT_FALSE(commit_or.ok());
  EXPECT_EQ(commit_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(TransactionManagerTest, ImplicitTransactionConflictRollsBackAndReturnsConflict) {
  MemoryStorage storage;

  TransactionManager::Options options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  options.conflict_options.max_attempts = 1;
  options.sleep_for = [](absl::Duration) {};
  TransactionManager manager(&storage, options);

  std::optional<std::string> seen_tx_id;
  auto implicit_or = manager.RunImplicitTransaction([&](const std::string& tx_id) {
    seen_tx_id = tx_id;
    // The tx_id IS the branch name — use directly.
    auto put_status = storage.PutObject(tx_id, "idx/non_unique/key", "tx-value");
    if (!put_status.ok()) {
      return put_status;
    }

    // Create a conflicting write on canonical while the transaction is open.
    auto canonical_put = storage.PutObject(storage.GetCanonicalBranch(), "idx/non_unique/key", "canonical-value");
    if (!canonical_put.ok()) {
      return canonical_put;
    }
    return storage.Commit(storage.GetCanonicalBranch(), "canonical update").status();
  });

  ASSERT_TRUE(implicit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitConflict>(*implicit_or));
  const auto& conflict = std::get<TransactionManager::CommitConflict>(*implicit_or);
  EXPECT_FALSE(conflict.detail.retryable());

  // Transaction should be cleaned up after conflict.
  ASSERT_TRUE(seen_tx_id.has_value());
  auto tx_meta_or = manager.GetTransactionMetadata(*seen_tx_id);
  ASSERT_FALSE(tx_meta_or.ok());
  EXPECT_EQ(tx_meta_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(TransactionManagerTest, NestedRollbackChildThenCommitParentSucceeds) {
  MemoryStorage storage;
  TransactionManager manager(&storage);

  auto parent_tx_id_or = manager.CreateTransaction();
  ASSERT_TRUE(parent_tx_id_or.ok());

  // The transaction_id IS the branch name — use directly.
  ASSERT_TRUE(storage.PutObject(*parent_tx_id_or, "parent/data.txt", "parent-data").ok());
  ASSERT_TRUE(storage.Commit(*parent_tx_id_or, "parent write").ok());

  // Create child transaction with parent_transaction_id.
  auto child_tx_id_or = manager.CreateTransaction(/*parent_snapshot_id=*/std::nullopt, /*parent_transaction_id=*/*parent_tx_id_or);
  ASSERT_TRUE(child_tx_id_or.ok());

  // The child transaction_id IS the branch name.
  ASSERT_TRUE(storage.PutObject(*child_tx_id_or, "child/data.txt", "child-data").ok());

  // Rollback the child — its writes should not propagate.
  ASSERT_TRUE(manager.RollbackTransaction(*child_tx_id_or).ok());

  // Parent should still be committable.
  auto parent_commit_or = manager.CommitTransaction(*parent_tx_id_or);
  ASSERT_TRUE(parent_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*parent_commit_or));

  // Parent's data should be on canonical.
  auto parent_read_or = storage.GetObject(storage.GetCanonicalBranch(), "parent/data.txt");
  ASSERT_TRUE(parent_read_or.ok());
  EXPECT_EQ(*parent_read_or, "parent-data");

  // Child's data should NOT be on canonical.
  auto child_read_or = storage.GetObject(storage.GetCanonicalBranch(), "child/data.txt");
  ASSERT_FALSE(child_read_or.ok());
  EXPECT_EQ(child_read_or.status().code(), absl::StatusCode::kNotFound);
}

// ---------------------------------------------------------------------------
// Multi-instance tests (C4 — Phase 12)
// Verify that two TransactionManager instances sharing the same Storage
// can correctly handle each other's transactions (statelessness validation).
// ---------------------------------------------------------------------------

TEST(TransactionManagerTest, MultiInstanceSnapshotCreatedOnOneInstanceVisibleOnAnother) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On manager A: create a transaction, write, commit, get snapshot.
  auto txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(txn_id_or.ok());
  ASSERT_TRUE(storage.PutObject(*txn_id_or, "multi/a.txt", "value-a").ok());
  ASSERT_TRUE(storage.Commit(*txn_id_or, "write from A").ok());

  auto commit_result_or = manager_a.CommitTransaction(*txn_id_or);
  ASSERT_TRUE(commit_result_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_result_or));
  const auto& commit_a = std::get<TransactionManager::CommitSuccess>(*commit_result_or);

  auto snapshot_a_or = manager_a.CreateSnapshot();
  ASSERT_TRUE(snapshot_a_or.ok());

  // On manager B: CreateSnapshot should return the same canonical head.
  auto snapshot_b_or = manager_b.CreateSnapshot();
  ASSERT_TRUE(snapshot_b_or.ok());

  // Both snapshots should refer to the same commit (canonical head).
  EXPECT_EQ(*snapshot_a_or, *snapshot_b_or);
  EXPECT_EQ(*snapshot_a_or, commit_a.snapshot_id);
}

TEST(TransactionManagerTest, MultiInstanceTransactionCreatedOnOneCommittedOnAnother) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On A: create a transaction.
  auto txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(txn_id_or.ok());
  const std::string txn_id = *txn_id_or;

  // Write objects on the transaction branch directly via storage.
  ASSERT_TRUE(storage.PutObject(txn_id, "cross/obj.txt", "cross-value").ok());
  ASSERT_TRUE(storage.Commit(txn_id, "write on A's txn").ok());

  // On B: CommitTransaction should succeed — B reads the metadata from storage.
  auto commit_result_or = manager_b.CommitTransaction(txn_id);
  ASSERT_TRUE(commit_result_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_result_or));

  // Verify the written objects are now on the canonical branch.
  auto read_or = storage.GetObject(storage.GetCanonicalBranch(), "cross/obj.txt");
  ASSERT_TRUE(read_or.ok());
  EXPECT_EQ(*read_or, "cross-value");
}

TEST(TransactionManagerTest, MultiInstanceTransactionRollbackOnDifferentInstance) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On A: create a transaction.
  auto txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(txn_id_or.ok());
  const std::string txn_id = *txn_id_or;

  // On B: rollback should succeed — B sees the branch exists.
  ASSERT_TRUE(manager_b.RollbackTransaction(txn_id).ok());

  // Verify the branch is deleted from storage.
  auto branch_head = storage.GetBranchHead(txn_id);
  ASSERT_FALSE(branch_head.ok());
  EXPECT_EQ(branch_head.status().code(), absl::StatusCode::kNotFound);

  // On B: GetTransactionMetadata should return NOT_FOUND.
  auto meta_b = manager_b.GetTransactionMetadata(txn_id);
  ASSERT_FALSE(meta_b.ok());
  EXPECT_EQ(meta_b.status().code(), absl::StatusCode::kNotFound);
}

TEST(TransactionManagerTest, MultiInstanceGetTransactionMetadata) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On A: create a transaction with no parent.
  auto txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(txn_id_or.ok());
  const std::string txn_id = *txn_id_or;

  // On B: GetTransactionMetadata should succeed.
  auto meta_b_or = manager_b.GetTransactionMetadata(txn_id);
  ASSERT_TRUE(meta_b_or.ok());
  EXPECT_EQ(meta_b_or->transaction_id, txn_id);
  EXPECT_EQ(meta_b_or->depth, 0);
}

TEST(TransactionManagerTest, MultiInstanceNestedTransactionOnDifferentInstance) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On A: create parent transaction.
  auto parent_txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(parent_txn_id_or.ok());
  const std::string parent_txn_id = *parent_txn_id_or;

  // On B: create child transaction with parent_transaction_id from A.
  auto child_txn_id_or = manager_b.CreateTransaction(
      /*parent_snapshot_id=*/std::nullopt,
      /*parent_transaction_id=*/parent_txn_id);
  ASSERT_TRUE(child_txn_id_or.ok());
  const std::string child_txn_id = *child_txn_id_or;

  // Write on child branch, commit child branch.
  ASSERT_TRUE(storage.PutObject(child_txn_id, "nested/cross.txt", "nested-cross-data").ok());
  ASSERT_TRUE(storage.Commit(child_txn_id, "child write from B").ok());

  // On B: CommitTransaction(child_txn_id) should merge into parent.
  auto child_commit_or = manager_b.CommitTransaction(child_txn_id);
  ASSERT_TRUE(child_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*child_commit_or));

  // Verify the child's data is on the parent branch.
  auto parent_read_or = storage.GetObject(parent_txn_id, "nested/cross.txt");
  ASSERT_TRUE(parent_read_or.ok());
  EXPECT_EQ(*parent_read_or, "nested-cross-data");

  // On A: CommitTransaction(parent_txn_id) should merge into canonical.
  auto parent_commit_or = manager_a.CommitTransaction(parent_txn_id);
  ASSERT_TRUE(parent_commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*parent_commit_or));

  // Verify the data is on canonical.
  auto canonical_read_or = storage.GetObject(storage.GetCanonicalBranch(), "nested/cross.txt");
  ASSERT_TRUE(canonical_read_or.ok());
  EXPECT_EQ(*canonical_read_or, "nested-cross-data");
}

TEST(TransactionManagerTest, MissingTxnMetadataDoesNotTreatNestedTransactionAsRoot) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  auto parent_txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(parent_txn_id_or.ok());

  auto child_txn_id_or = manager_a.CreateTransaction(
      /*parent_snapshot_id=*/std::nullopt,
      /*parent_transaction_id=*/*parent_txn_id_or);
  ASSERT_TRUE(child_txn_id_or.ok());

  ASSERT_TRUE(storage.PutObject(*child_txn_id_or, "nested/missing-meta.txt", "child-data").ok());
  ASSERT_TRUE(storage.Commit(*child_txn_id_or, "child write").ok());

  ASSERT_TRUE(storage.DeleteObject(*child_txn_id_or, absl::StrCat("_txn_meta/", *child_txn_id_or)).ok());
  ASSERT_TRUE(storage.Commit(*child_txn_id_or, "simulate missing txn metadata").ok());

  auto commit_or = manager_b.CommitTransaction(*child_txn_id_or);
  ASSERT_FALSE(commit_or.ok());
  EXPECT_EQ(commit_or.status().code(), absl::StatusCode::kFailedPrecondition);

  auto canonical_read_or = storage.GetObject(storage.GetCanonicalBranch(), "nested/missing-meta.txt");
  ASSERT_FALSE(canonical_read_or.ok());
  EXPECT_EQ(canonical_read_or.status().code(), absl::StatusCode::kNotFound);

  auto parent_read_or = storage.GetObject(*parent_txn_id_or, "nested/missing-meta.txt");
  ASSERT_FALSE(parent_read_or.ok());
  EXPECT_EQ(parent_read_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(TransactionManagerTest, CreateNestedTransactionFailsWhenParentMetadataMissing) {
  MemoryStorage storage;

  TransactionManager manager_a(&storage);
  TransactionManager manager_b(&storage);

  auto parent_txn_id_or = manager_a.CreateTransaction();
  ASSERT_TRUE(parent_txn_id_or.ok());

  ASSERT_TRUE(storage.DeleteObject(*parent_txn_id_or, absl::StrCat("_txn_meta/", *parent_txn_id_or)).ok());
  ASSERT_TRUE(storage.Commit(*parent_txn_id_or, "remove parent txn metadata").ok());

  auto child_txn_id_or = manager_b.CreateTransaction(
      /*parent_snapshot_id=*/std::nullopt,
      /*parent_transaction_id=*/*parent_txn_id_or);
  ASSERT_FALSE(child_txn_id_or.ok());
  EXPECT_EQ(child_txn_id_or.status().code(), absl::StatusCode::kFailedPrecondition);
}

TEST(TransactionManagerTest, MultiInstanceGetSnapshotMetadata) {
  MemoryStorage storage;

  TransactionManager::Options opts;
  opts.sleep_for = [](absl::Duration) {};

  TransactionManager manager_a(&storage, opts);
  TransactionManager manager_b(&storage, opts);

  // On A: create a snapshot.
  auto snapshot_id_or = manager_a.CreateSnapshot();
  ASSERT_TRUE(snapshot_id_or.ok());
  const std::string snapshot_id = *snapshot_id_or;

  // On B: GetSnapshotMetadata should succeed.
  auto meta_b_or = manager_b.GetSnapshotMetadata(snapshot_id);
  ASSERT_TRUE(meta_b_or.ok());
  EXPECT_EQ(meta_b_or->snapshot_id, snapshot_id);
  // source_transaction_id may be empty cross-instance (best-effort cache).
  // We do not assert on source_transaction_id.
}

} // namespace
} // namespace artifact_system::testing

#include "transaction/transaction_manager.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, index_path, "ours").ok());

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
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, "payload/object-1", "txn-value").ok());
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
  auto transaction_meta_or = manager.GetTransactionMetadata(*transaction_id_or);
  ASSERT_TRUE(transaction_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(transaction_meta_or->branch_name, index_path, "txn-value").ok());
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

  std::optional<uint64_t> seen_tx_id;
  auto implicit_or = manager.RunImplicitTransaction([&](uint64_t tx_id) {
    seen_tx_id = tx_id;
    auto tx_meta_or = manager.GetTransactionMetadata(tx_id);
    if (!tx_meta_or.ok()) {
      return tx_meta_or.status();
    }
    auto put_status = storage.PutObject(tx_meta_or->branch_name, "idx/non_unique/key", "tx-value");
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
  auto parent_meta_or = manager.GetTransactionMetadata(*parent_tx_id_or);
  ASSERT_TRUE(parent_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(parent_meta_or->branch_name, "parent/data.txt", "parent-data").ok());

  auto child_tx_id_or = manager.CreateTransaction(*parent_tx_id_or);
  ASSERT_TRUE(child_tx_id_or.ok());
  auto child_meta_or = manager.GetTransactionMetadata(*child_tx_id_or);
  ASSERT_TRUE(child_meta_or.ok());

  ASSERT_TRUE(storage.PutObject(child_meta_or->branch_name, "child/data.txt", "child-data").ok());

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

} // namespace
} // namespace artifact_system::testing

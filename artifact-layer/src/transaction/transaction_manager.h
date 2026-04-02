#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "artifact_service.pb.h"
#include "storage/storage_interface.h"
#include "transaction/conflict_resolver.h"

namespace artifact_system::artifact {
class ArtifactStore;
}

namespace artifact_system::transaction {

class TransactionManager {
public:
  // Configuration for writing TransactionCommitRecord artifacts on commit.
  // When present, CommitTransaction writes a record for user-initiated commits.
  // The artifact_store must be configured with bypass_mutation_check=true.
  struct CommitRecordConfig {
    // Artifact ID of the TransactionCommitRecord TypeVersionDefinition.
    uint64_t version_def_id = 0;
    // ArtifactStore with bypass_mutation_check=true for creating records.
    artifact::ArtifactStore* artifact_store = nullptr;
  };

  struct Options {
    ConflictResolverOptions conflict_options = {
        .max_attempts = 5,
        .initial_backoff = absl::Milliseconds(100),
        .max_backoff = absl::Seconds(2),
    };
    PathConflictClassifier path_conflict_classifier;
    RetryConflictResolver retry_conflict_resolver;
    std::function<void(absl::Duration)> sleep_for;
    std::function<void(const absl::Status&)> on_cleanup_failure;
    std::optional<CommitRecordConfig> commit_record_config;
  };

  explicit TransactionManager(StorageInterface* storage);
  TransactionManager(StorageInterface* storage, Options options);

  struct SnapshotMetadata {
    std::string snapshot_id;
    std::optional<std::string> source_transaction_id;
  };

  struct TransactionMetadata {
    std::string transaction_id;
    std::optional<std::string> parent_transaction_id;
    std::optional<std::string> parent_snapshot_id;
    uint32_t depth = 0;
  };

  struct CommitSuccess {
    std::string transaction_id;
    std::string commit_id;
    std::string snapshot_id;
  };

  struct CommitConflict {
    std::string transaction_id;
    MergeResult::Conflict conflict;
    artifact_system::CommitConflict detail;
  };

  using CommitResult = std::variant<CommitSuccess, CommitConflict>;
  using ImplicitTransactionCallback = std::function<absl::Status(const std::string&)>;

  absl::StatusOr<std::string> CreateSnapshot(std::optional<std::string> parent_transaction_id = std::nullopt);
  absl::StatusOr<std::string> CreateTransaction(std::optional<std::string> parent_snapshot_id = std::nullopt,
                                                std::optional<std::string> parent_transaction_id = std::nullopt);
  absl::StatusOr<CommitResult> CommitTransaction(const std::string& transaction_id);
  absl::Status RollbackTransaction(const std::string& transaction_id);

  absl::StatusOr<CommitResult> RunImplicitTransaction(const ImplicitTransactionCallback& callback, std::optional<std::string> parent_id = std::nullopt);

  void SetCommitRecordConfig(CommitRecordConfig config);

  absl::StatusOr<SnapshotMetadata> GetSnapshotMetadata(const std::string& snapshot_id) const;
  absl::StatusOr<TransactionMetadata> GetTransactionMetadata(const std::string& transaction_id) const;

private:
  struct SnapshotSource {
    std::optional<std::string> source_transaction_id;
  };

  struct TransactionRecord {
    std::optional<std::string> parent_transaction_id;
    std::optional<std::string> parent_snapshot_id;
    uint32_t depth = 0;
    // Local-only: tracks child transactions created on THIS instance.
    // Cannot detect children created on other instances.
    std::set<std::string> child_transaction_ids;
  };

  std::string GenerateBranchName();

  // Transaction metadata persistence helpers.
  // Metadata is stored at _txn_meta/{transaction_id} on the branch.
  static std::string TxnMetaPath(const std::string& transaction_id);
  absl::Status WriteTxnMeta(const std::string& branch, const TransactionRecord& record);
  absl::StatusOr<TransactionRecord> LoadTxnMeta(const std::string& branch) const;
  absl::Status CleanupTxnMeta(const std::string& branch);

  // Load transaction record from cache or Storage Layer.
  // On cache miss, reads _txn_meta from the branch and populates cache.
  absl::StatusOr<TransactionRecord> ResolveTransaction(const std::string& transaction_id);

  absl::Status RemoveTransactionLocked(const std::string& transaction_id);
  absl::StatusOr<CommitResult> CommitTransactionImpl(const std::string& transaction_id, bool write_commit_record);

  StorageInterface* storage_;
  Options options_;

  mutable std::mutex mutex_;
  // Cache: source_transaction_id for snapshots created on this instance.
  // Best-effort — not required for correctness.
  std::unordered_map<std::string, SnapshotSource> snapshot_source_cache_;
  // Cache: transaction records for transactions known to this instance.
  // On cache miss, the Storage Layer is queried (branch existence + _txn_meta).
  std::unordered_map<std::string, TransactionRecord> transactions_;
};

} // namespace artifact_system::transaction

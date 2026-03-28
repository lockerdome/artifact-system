#pragma once

#include <cstdint>
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

namespace artifact_system::transaction {

class TransactionManager {
public:
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
  };

  explicit TransactionManager(StorageInterface* storage);
  TransactionManager(StorageInterface* storage, Options options);

  struct SnapshotMetadata {
    uint64_t snapshot_id = 0;
    std::string commit_id;
    std::optional<uint64_t> source_transaction_id;
  };

  struct TransactionMetadata {
    uint64_t transaction_id = 0;
    std::string branch_name;
    std::optional<uint64_t> parent_transaction_id;
    std::optional<uint64_t> parent_snapshot_id;
    uint32_t depth = 0;
  };

  struct CommitSuccess {
    uint64_t transaction_id = 0;
    std::string commit_id;
    std::optional<uint64_t> snapshot_id;
  };

  struct CommitConflict {
    uint64_t transaction_id = 0;
    MergeResult::Conflict conflict;
    artifact_system::CommitConflict detail;
  };

  using CommitResult = std::variant<CommitSuccess, CommitConflict>;
  using ImplicitTransactionCallback = std::function<absl::Status(uint64_t)>;

  absl::StatusOr<uint64_t> CreateSnapshot(std::optional<uint64_t> parent_transaction_id = std::nullopt);
  absl::StatusOr<uint64_t> CreateTransaction(std::optional<uint64_t> parent_id = std::nullopt);
  absl::StatusOr<CommitResult> CommitTransaction(uint64_t transaction_id);
  absl::Status RollbackTransaction(uint64_t transaction_id);

  absl::StatusOr<CommitResult> RunImplicitTransaction(const ImplicitTransactionCallback& callback, std::optional<uint64_t> parent_id = std::nullopt);

  absl::StatusOr<SnapshotMetadata> GetSnapshotMetadata(uint64_t snapshot_id) const;
  absl::StatusOr<TransactionMetadata> GetTransactionMetadata(uint64_t transaction_id) const;

private:
  struct SnapshotRecord {
    std::string commit_id;
    std::optional<uint64_t> source_transaction_id;
  };

  struct TransactionRecord {
    std::string branch_name;
    std::optional<uint64_t> parent_transaction_id;
    std::optional<uint64_t> parent_snapshot_id;
    uint32_t depth = 0;
    std::set<uint64_t> child_transaction_ids;
  };

  uint64_t NextIdLocked();
  absl::Status RemoveTransactionLocked(uint64_t transaction_id);

  StorageInterface* storage_;
  Options options_;

  mutable std::mutex mutex_;
  uint64_t next_id_ = 1;
  std::unordered_map<uint64_t, SnapshotRecord> snapshots_;
  std::unordered_map<uint64_t, TransactionRecord> transactions_;
};

} // namespace artifact_system::transaction

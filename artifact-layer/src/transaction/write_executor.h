#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "artifact_service.pb.h"
#include "storage/storage_interface.h"
#include "transaction/conflict_resolver.h"

namespace artifact_system::transaction {

struct WriteExecutorOptions {
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

class WriteExecutor {
public:
  struct WriteSuccess {
    std::string commit_id;
    uint32_t attempts = 0;
  };

  struct WriteConflict {
    MergeResult::Conflict conflict;
    CommitConflict detail;
    uint32_t attempts = 0;
  };

  using WriteResult = std::variant<WriteSuccess, WriteConflict>;
  using StagingCallback = std::function<absl::Status(const std::string&)>;

  explicit WriteExecutor(StorageInterface* storage, WriteExecutorOptions options = {});

  absl::StatusOr<WriteResult> ExecuteWrite(const std::string& transaction_branch, const StagingCallback& staging_callback);

private:
  std::string NextChildBranchName(const std::string& transaction_branch);

  StorageInterface* storage_ = nullptr;
  WriteExecutorOptions options_;
};

} // namespace artifact_system::transaction

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "storage/storage_interface.h"

namespace artifact_system {

class MemoryStorage final : public StorageInterface {
public:
  MemoryStorage();

  // Branch operations
  absl::StatusOr<std::string> CreateBranch(const std::string& name, const std::string& base_commit_id) override;
  absl::Status DeleteBranch(const std::string& branch) override;
  absl::StatusOr<std::string> GetBranchHead(const std::string& branch) override;

  // Object I/O
  absl::Status PutObject(const std::string& branch, const std::string& path, const std::string& data) override;
  absl::StatusOr<std::string> GetObject(const std::string& ref, const std::string& path) override;
  absl::Status DeleteObject(const std::string& branch, const std::string& path) override;
  absl::StatusOr<bool> ObjectExists(const std::string& ref, const std::string& path) override;
  absl::StatusOr<std::vector<std::string>> ListObjects(const std::string& ref, const std::string& prefix) override;

  // Commit and merge
  absl::StatusOr<std::string> Commit(const std::string& branch, const std::string& message) override;
  absl::StatusOr<MergeResult> Merge(const std::string& source, const std::string& target) override;

  // Canonical branch
  std::string GetCanonicalBranch() const override;

private:
  struct CommitData {
    std::string id;
    std::string message;
    std::vector<std::string> parent_ids;
    std::map<std::string, std::string> objects; // path -> data (full snapshot)
  };

  struct StagedChange {
    std::optional<std::string> data; // nullopt = delete tombstone
  };

  struct BranchData {
    std::string head_commit_id;
    std::map<std::string, StagedChange> staging;
  };

  /// Resolve the effective state of a branch (committed + staged).
  std::map<std::string, std::string> ResolveState(const BranchData& branch) const;

  /// Resolve a ref (branch or commit) to an object state snapshot.
  absl::StatusOr<std::map<std::string, std::string>> ResolveRefState(const std::string& ref) const;

  /// Find the best common ancestor of two commits.
  absl::StatusOr<std::string> FindMergeBase(const std::string& commit_a, const std::string& commit_b) const;

  /// Generate a new commit ID.
  std::string NextCommitId();

  static constexpr const char* kCanonicalBranch = "main";

  std::map<std::string, CommitData> commits_;
  std::map<std::string, BranchData> branches_;
  uint64_t next_commit_seq_ = 0;
};

} // namespace artifact_system

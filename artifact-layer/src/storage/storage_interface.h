#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace artifact_system {

/// Result of a merge operation.
struct MergeResult {
  struct Success {
    std::string commit_id; // Commit ID of the merge commit
  };

  struct Conflict {
    std::vector<std::string> conflicting_paths; // Object paths that conflict
    std::string base_commit_id;                 // Merge base commit (for 3-way diff)
    std::string source_commit_id;               // Source branch head at merge time
    std::string target_commit_id;               // Target branch head at merge time
  };

  std::variant<Success, Conflict> result;

  bool IsSuccess() const {
    return std::holds_alternative<Success>(result);
  }
  bool IsConflict() const {
    return std::holds_alternative<Conflict>(result);
  }

  const Success& GetSuccess() const {
    return std::get<Success>(result);
  }
  const Conflict& GetConflict() const {
    return std::get<Conflict>(result);
  }
};

/// Abstract interface for the Storage Layer.
///
/// The Storage Layer provides durable object storage with versioned commits,
/// branches, merges, and conflict detection. LakeFS is the production
/// implementation; MemoryStorage is the test implementation.
///
/// All branch operations, object I/O, and commit/merge use string identifiers
/// for branches and commits.
class StorageInterface {
public:
  virtual ~StorageInterface() = default;

  // ── Branch operations ──────────────────────────────────────────────────

  /// Create a new branch from a base commit.
  /// @param name Branch name (must be unique).
  /// @param base_commit_id The commit to fork from. Empty string means fork
  ///        from the canonical branch head.
  /// @return The branch name (same as input) on success.
  virtual absl::StatusOr<std::string> CreateBranch(const std::string& name, const std::string& base_commit_id) = 0;

  /// Delete a branch.
  /// @param branch The branch name to delete.
  /// @return OK on success, NOT_FOUND if branch doesn't exist.
  ///         Cannot delete the canonical branch.
  virtual absl::Status DeleteBranch(const std::string& branch) = 0;

  /// Get the head commit of a branch.
  /// @param branch The branch name.
  /// @return The commit ID of the branch head.
  virtual absl::StatusOr<std::string> GetBranchHead(const std::string& branch) = 0;

  // ── Object I/O ─────────────────────────────────────────────────────────

  /// Write an object to a branch's staging area.
  /// Overwrites if the path already exists.
  virtual absl::Status PutObject(const std::string& branch, const std::string& path, const std::string& data) = 0;

  /// Read an object from a ref.
  /// Ref may be either a branch name or a commit ID.
  /// For branches, staged changes are visible.
  /// @return The object data, or NOT_FOUND if the path doesn't exist.
  virtual absl::StatusOr<std::string> GetObject(const std::string& ref, const std::string& path) = 0;

  /// Delete an object from a branch's staging area.
  /// The delete is staged (not committed until Commit is called).
  virtual absl::Status DeleteObject(const std::string& branch, const std::string& path) = 0;

  /// Check if an object exists on a ref.
  /// Ref may be either a branch name or a commit ID.
  /// For branches, staged changes are visible.
  virtual absl::StatusOr<bool> ObjectExists(const std::string& ref, const std::string& path) = 0;

  /// List all object paths under a prefix on a ref.
  /// Ref may be either a branch name or a commit ID.
  /// For branches, staged changes are visible.
  /// Returns paths in sorted order.
  virtual absl::StatusOr<std::vector<std::string>> ListObjects(const std::string& ref, const std::string& prefix) = 0;

  // ── Commit and merge ───────────────────────────────────────────────────

  /// Commit all staged changes on a branch.
  /// @param branch The branch to commit on.
  /// @param message Commit message (for debugging/audit).
  /// @return The new commit ID.
  virtual absl::StatusOr<std::string> Commit(const std::string& branch, const std::string& message) = 0;

  /// Merge source branch into target branch.
  /// Uses three-way merge with the common ancestor as base.
  /// @return MergeResult indicating success or conflicts.
  virtual absl::StatusOr<MergeResult> Merge(const std::string& source, const std::string& target) = 0;

  // ── Canonical branch ───────────────────────────────────────────────────

  /// Get the name of the canonical (main) branch.
  virtual std::string GetCanonicalBranch() const = 0;
};

} // namespace artifact_system

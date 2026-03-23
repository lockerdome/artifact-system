#pragma once

#include <memory>
#include <string>
#include <vector>

#include "storage/lakefs_config.h"
#include "storage/storage_interface.h"

namespace artifact_system {

class LakeFSStorage final : public StorageInterface {
public:
  /// Construct a LakeFS storage client.
  /// The config is copied and used for subsequent API calls.
  explicit LakeFSStorage(LakeFSConfig config);
  ~LakeFSStorage() override;

  // Not copyable, but movable.
  LakeFSStorage(const LakeFSStorage&) = delete;
  LakeFSStorage& operator=(const LakeFSStorage&) = delete;
  LakeFSStorage(LakeFSStorage&& other) noexcept = default;
  LakeFSStorage& operator=(LakeFSStorage&& other) noexcept = default;

  // ── Branch operations ──────────────────────────────────────────────────

  absl::StatusOr<std::string> CreateBranch(const std::string& name, const std::string& base_commit_id) override;
  absl::Status DeleteBranch(const std::string& branch) override;
  absl::StatusOr<std::string> GetBranchHead(const std::string& branch) override;

  // ── Object I/O ─────────────────────────────────────────────────────────

  absl::Status PutObject(const std::string& branch, const std::string& path, const std::string& data) override;
  absl::StatusOr<std::string> GetObject(const std::string& ref, const std::string& path) override;
  absl::Status DeleteObject(const std::string& branch, const std::string& path) override;
  absl::StatusOr<bool> ObjectExists(const std::string& ref, const std::string& path) override;
  absl::StatusOr<std::vector<std::string>> ListObjects(const std::string& ref, const std::string& prefix) override;

  // ── Commit and merge ───────────────────────────────────────────────────

  absl::StatusOr<std::string> Commit(const std::string& branch, const std::string& message) override;
  absl::StatusOr<MergeResult> Merge(const std::string& source, const std::string& target) override;

  // ── Canonical branch ───────────────────────────────────────────────────

  std::string GetCanonicalBranch() const override;

private:
  /// HTTP response from a LakeFS API call.
  struct HttpResponse {
    long status_code = 0;
    std::string body;
  };

  /// Perform an HTTP request against the LakeFS API.
  /// @param method HTTP method (GET, POST, PUT, DELETE).
  /// @param api_path Path relative to /api/v1 (e.g., "/repositories/myrepo/branches").
  /// @param body Optional request body (JSON string for most endpoints).
  /// @param content_type Content-Type header value. Defaults to "application/json".
  /// @param accept_type Accept header value. Defaults to "application/json".
  HttpResponse DoRequest(const std::string& method, const std::string& api_path, const std::string& body = "",
                         const std::string& content_type = "application/json", const std::string& accept_type = "application/json");

  /// Upload an object using multipart form POST (LakeFS object upload API).
  HttpResponse DoUpload(const std::string& api_path, const std::string& data);

  /// Build the full API URL for a given path.
  std::string ApiUrl(const std::string& api_path) const;

  /// URL-encode a path component.
  static std::string UrlEncode(const std::string& value);

  /// Check if a branch exists. Returns true if found, false if not.
  absl::StatusOr<bool> BranchExists(const std::string& branch);

  /// Check if a commit exists. Returns true if found, false if not.
  absl::StatusOr<bool> CommitExists(const std::string& commit_id);

  /// Validate a ref and classify whether it resolves to a branch.
  /// Returns true if ref is a branch, false if ref is a commit, or NOT_FOUND
  /// if no branch/commit matches the ref.
  absl::StatusOr<bool> ResolveRefKind(const std::string& ref);

  /// Check if a ref has uncommitted changes (staging area is non-empty).
  absl::StatusOr<bool> HasUncommittedChanges(const std::string& branch);

  /// Find the merge base between two refs.
  absl::StatusOr<std::string> FindMergeBase(const std::string& source_ref, const std::string& dest_ref);

  /// Get the diff between two refs, filtering for conflicts.
  absl::StatusOr<std::vector<std::string>> GetConflictingPaths(const std::string& source_ref, const std::string& dest_ref);

  /// Get object stats (metadata) at a ref. Returns NOT_FOUND status if object
  /// does not exist.
  absl::StatusOr<HttpResponse> StatObject(const std::string& ref, const std::string& path);

  LakeFSConfig config_;
};

} // namespace artifact_system

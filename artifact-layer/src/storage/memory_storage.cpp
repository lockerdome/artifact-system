#include "storage/memory_storage.h"

#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace artifact_system {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MemoryStorage::MemoryStorage() {
  // Create the genesis commit (empty snapshot).
  CommitData genesis;
  genesis.id = NextCommitId(); // "commit-0"
  genesis.message = "genesis";
  // No parents, no objects.
  commits_[genesis.id] = std::move(genesis);

  // Create the canonical branch pointing at the genesis commit.
  BranchData main_branch;
  main_branch.head_commit_id = "commit-0";
  branches_[kCanonicalBranch] = std::move(main_branch);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string MemoryStorage::NextCommitId() {
  return absl::StrCat("commit-", next_commit_seq_++);
}

std::map<std::string, std::string> MemoryStorage::ResolveState(const BranchData& branch) const {
  // Start from the committed snapshot.
  auto it = commits_.find(branch.head_commit_id);
  std::map<std::string, std::string> state;
  if (it != commits_.end()) {
    state = it->second.objects;
  }

  // Apply staged changes.
  for (const auto& [path, change] : branch.staging) {
    if (change.data.has_value()) {
      state[path] = *change.data;
    } else {
      // Tombstone — remove the path.
      state.erase(path);
    }
  }
  return state;
}

std::string MemoryStorage::FindMergeBase(const std::string& commit_a, const std::string& commit_b) const {
  // BFS from both commits simultaneously.  The first commit found in both
  // ancestor sets is the merge base.
  std::set<std::string> ancestors_a;
  std::set<std::string> ancestors_b;
  std::deque<std::string> queue_a;
  std::deque<std::string> queue_b;

  queue_a.push_back(commit_a);
  ancestors_a.insert(commit_a);
  queue_b.push_back(commit_b);
  ancestors_b.insert(commit_b);

  if (commit_a == commit_b) {
    return commit_a;
  }

  // Alternating BFS expansion.
  while (!queue_a.empty() || !queue_b.empty()) {
    // Expand one level from A.
    if (!queue_a.empty()) {
      auto current = queue_a.front();
      queue_a.pop_front();
      auto cit = commits_.find(current);
      if (cit != commits_.end()) {
        for (const auto& parent : cit->second.parent_ids) {
          if (ancestors_b.contains(parent)) {
            return parent;
          }
          if (ancestors_a.insert(parent).second) {
            queue_a.push_back(parent);
          }
        }
      }
    }

    // Expand one level from B.
    if (!queue_b.empty()) {
      auto current = queue_b.front();
      queue_b.pop_front();
      auto cit = commits_.find(current);
      if (cit != commits_.end()) {
        for (const auto& parent : cit->second.parent_ids) {
          if (ancestors_a.contains(parent)) {
            return parent;
          }
          if (ancestors_b.insert(parent).second) {
            queue_b.push_back(parent);
          }
        }
      }
    }
  }

  // Fallback: should not happen if all commits share the genesis.
  return "commit-0";
}

// ---------------------------------------------------------------------------
// Branch operations
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> MemoryStorage::CreateBranch(const std::string& name, const std::string& base_commit_id) {
  if (branches_.contains(name)) {
    return absl::AlreadyExistsError(absl::StrCat("branch already exists: ", name));
  }

  std::string base = base_commit_id;
  if (base.empty()) {
    // Fork from the canonical branch head.
    auto it = branches_.find(kCanonicalBranch);
    if (it == branches_.end()) {
      return absl::InternalError("canonical branch not found");
    }
    base = it->second.head_commit_id;
  }

  if (!commits_.contains(base)) {
    return absl::NotFoundError(absl::StrCat("base commit not found: ", base));
  }

  BranchData branch;
  branch.head_commit_id = base;
  branches_[name] = std::move(branch);
  return name;
}

absl::Status MemoryStorage::DeleteBranch(const std::string& branch) {
  if (branch == kCanonicalBranch) {
    return absl::FailedPreconditionError("cannot delete canonical branch");
  }
  auto it = branches_.find(branch);
  if (it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  branches_.erase(it);
  return absl::OkStatus();
}

absl::StatusOr<std::string> MemoryStorage::GetBranchHead(const std::string& branch) {
  auto it = branches_.find(branch);
  if (it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  return it->second.head_commit_id;
}

// ---------------------------------------------------------------------------
// Object I/O
// ---------------------------------------------------------------------------

absl::Status MemoryStorage::PutObject(const std::string& branch, const std::string& path, const std::string& data) {
  auto it = branches_.find(branch);
  if (it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  it->second.staging[path] = StagedChange{data};
  return absl::OkStatus();
}

absl::StatusOr<std::string> MemoryStorage::GetObject(const std::string& branch, const std::string& path) {
  auto bit = branches_.find(branch);
  if (bit == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }

  const auto& br = bit->second;

  // Check staging first.
  auto sit = br.staging.find(path);
  if (sit != br.staging.end()) {
    if (sit->second.data.has_value()) {
      return *sit->second.data;
    }
    // Tombstone — staged delete.
    return absl::NotFoundError(absl::StrCat("object not found: ", path));
  }

  // Fall through to committed state.
  auto cit = commits_.find(br.head_commit_id);
  if (cit == commits_.end()) {
    return absl::InternalError("head commit not found");
  }
  auto oit = cit->second.objects.find(path);
  if (oit == cit->second.objects.end()) {
    return absl::NotFoundError(absl::StrCat("object not found: ", path));
  }
  return oit->second;
}

absl::StatusOr<std::string> MemoryStorage::GetObjectAtCommit(const std::string& commit_id, const std::string& path) {
  auto cit = commits_.find(commit_id);
  if (cit == commits_.end()) {
    return absl::NotFoundError(absl::StrCat("commit not found: ", commit_id));
  }
  auto oit = cit->second.objects.find(path);
  if (oit == cit->second.objects.end()) {
    return absl::NotFoundError(absl::StrCat("object not found at commit ", commit_id, ": ", path));
  }
  return oit->second;
}

absl::Status MemoryStorage::DeleteObject(const std::string& branch, const std::string& path) {
  auto it = branches_.find(branch);
  if (it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  // Stage a tombstone regardless of whether the path currently exists.
  it->second.staging[path] = StagedChange{std::nullopt};
  return absl::OkStatus();
}

absl::StatusOr<bool> MemoryStorage::ObjectExists(const std::string& branch, const std::string& path) {
  auto bit = branches_.find(branch);
  if (bit == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }

  const auto& br = bit->second;

  // Check staging first.
  auto sit = br.staging.find(path);
  if (sit != br.staging.end()) {
    return sit->second.data.has_value();
  }

  // Fall through to committed state.
  auto cit = commits_.find(br.head_commit_id);
  if (cit == commits_.end()) {
    return absl::InternalError("head commit not found");
  }
  return cit->second.objects.contains(path);
}

absl::StatusOr<bool> MemoryStorage::ObjectExistsAtCommit(const std::string& commit_id, const std::string& path) {
  auto cit = commits_.find(commit_id);
  if (cit == commits_.end()) {
    return absl::NotFoundError(absl::StrCat("commit not found: ", commit_id));
  }
  return cit->second.objects.contains(path);
}

absl::StatusOr<std::vector<std::string>> MemoryStorage::ListObjects(const std::string& branch, const std::string& prefix) {
  auto bit = branches_.find(branch);
  if (bit == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }

  auto state = ResolveState(bit->second);
  std::vector<std::string> result;
  for (const auto& [path, _] : state) {
    if (path.starts_with(prefix)) {
      result.push_back(path);
    }
  }
  // state is a std::map so paths are already sorted.
  return result;
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> MemoryStorage::Commit(const std::string& branch, const std::string& message) {
  auto bit = branches_.find(branch);
  if (bit == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }

  auto& br = bit->second;

  // Build the new snapshot from committed + staged.
  auto snapshot = ResolveState(br);

  CommitData commit;
  commit.id = NextCommitId();
  commit.message = message;
  commit.parent_ids.push_back(br.head_commit_id);
  commit.objects = std::move(snapshot);

  std::string commit_id = commit.id;
  commits_[commit_id] = std::move(commit);

  // Advance head and clear staging.
  br.head_commit_id = commit_id;
  br.staging.clear();

  return commit_id;
}

// ---------------------------------------------------------------------------
// Merge
// ---------------------------------------------------------------------------

absl::StatusOr<MergeResult> MemoryStorage::Merge(const std::string& source, const std::string& target) {
  // Validate branches exist.
  auto src_it = branches_.find(source);
  if (src_it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("source branch not found: ", source));
  }
  auto tgt_it = branches_.find(target);
  if (tgt_it == branches_.end()) {
    return absl::NotFoundError(absl::StrCat("target branch not found: ", target));
  }

  // Both branches must have empty staging areas.
  if (!src_it->second.staging.empty()) {
    return absl::FailedPreconditionError(absl::StrCat("source branch '", source, "' has uncommitted changes"));
  }
  if (!tgt_it->second.staging.empty()) {
    return absl::FailedPreconditionError(absl::StrCat("target branch '", target, "' has uncommitted changes"));
  }

  // Copy head commit IDs to avoid holding references into branches_ map
  // across potential modifications (defensive against future refactoring).
  const std::string source_head = src_it->second.head_commit_id;
  const std::string target_head = tgt_it->second.head_commit_id;

  // Find the common ancestor.
  std::string base_id = FindMergeBase(source_head, target_head);

  // Get snapshots (use find() instead of at() to avoid throwing on bad state).
  auto base_it = commits_.find(base_id);
  if (base_it == commits_.end()) {
    return absl::InternalError(absl::StrCat("merge base commit not found: ", base_id));
  }
  auto source_commit_it = commits_.find(source_head);
  if (source_commit_it == commits_.end()) {
    return absl::InternalError(absl::StrCat("source head commit not found: ", source_head));
  }
  auto target_commit_it = commits_.find(target_head);
  if (target_commit_it == commits_.end()) {
    return absl::InternalError(absl::StrCat("target head commit not found: ", target_head));
  }
  const auto& base_objects = base_it->second.objects;
  const auto& source_objects = source_commit_it->second.objects;
  const auto& target_objects = target_commit_it->second.objects;

  // Collect all paths across all three snapshots.
  std::set<std::string> all_paths;
  for (const auto& [p, _] : base_objects)
    all_paths.insert(p);
  for (const auto& [p, _] : source_objects)
    all_paths.insert(p);
  for (const auto& [p, _] : target_objects)
    all_paths.insert(p);

  // Three-way merge.
  std::map<std::string, std::string> merged;
  std::vector<std::string> conflicts;

  for (const auto& path : all_paths) {
    // Get state in each snapshot (nullopt = path does not exist).
    auto get_val = [&](const std::map<std::string, std::string>& m) -> std::optional<std::string> {
      auto it = m.find(path);
      if (it != m.end())
        return it->second;
      return std::nullopt;
    };

    auto base_val = get_val(base_objects);
    auto source_val = get_val(source_objects);
    auto target_val = get_val(target_objects);

    bool source_changed = (source_val != base_val);
    bool target_changed = (target_val != base_val);

    if (source_changed && target_changed) {
      // Both sides changed relative to base.
      if (source_val == target_val) {
        // Identical change — no conflict.
        if (source_val.has_value()) {
          merged[path] = *source_val;
        }
        // If both deleted, path stays absent.
      } else {
        // Divergent changes — conflict.
        conflicts.push_back(path);
      }
    } else if (source_changed) {
      // Only source changed.
      if (source_val.has_value()) {
        merged[path] = *source_val;
      }
      // If source deleted, path stays absent.
    } else if (target_changed) {
      // Only target changed.
      if (target_val.has_value()) {
        merged[path] = *target_val;
      }
      // If target deleted, path stays absent.
    } else {
      // Neither side changed — keep base state.
      if (base_val.has_value()) {
        merged[path] = *base_val;
      }
    }
  }

  if (!conflicts.empty()) {
    std::sort(conflicts.begin(), conflicts.end());
    MergeResult result;
    result.result = MergeResult::Conflict{
        .conflicting_paths = std::move(conflicts),
        .base_commit_id = base_id,
        .source_commit_id = source_head,
        .target_commit_id = target_head,
    };
    return result;
  }

  // Create a merge commit on the target branch.
  CommitData merge_commit;
  merge_commit.id = NextCommitId();
  merge_commit.message = absl::StrCat("Merge '", source, "' into '", target, "'");
  merge_commit.parent_ids.push_back(target_head);
  merge_commit.parent_ids.push_back(source_head);
  merge_commit.objects = std::move(merged);

  std::string merge_commit_id = merge_commit.id;
  commits_[merge_commit_id] = std::move(merge_commit);

  // Advance target branch head.
  tgt_it->second.head_commit_id = merge_commit_id;

  MergeResult result;
  result.result = MergeResult::Success{.commit_id = merge_commit_id};
  return result;
}

// ---------------------------------------------------------------------------
// Canonical branch
// ---------------------------------------------------------------------------

std::string MemoryStorage::GetCanonicalBranch() const {
  return kCanonicalBranch;
}

} // namespace artifact_system

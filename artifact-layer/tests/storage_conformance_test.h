#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"

#include "storage/storage_interface.h"

namespace artifact_system::testing {

// ---------------------------------------------------------------------------
// Typed-test fixture for StorageInterface conformance.
//
// Concrete test files define a factory struct with:
//
//   struct MyFactory {
//     static std::unique_ptr<StorageInterface> Create();
//   };
//
// and then instantiate:
//
//   INSTANTIATE_TYPED_TEST_SUITE_P(MyPrefix, StorageConformanceTest, MyFactory);
// ---------------------------------------------------------------------------
template <typename T> class StorageConformanceTest : public ::testing::Test {
protected:
  void SetUp() override {
    storage_ = T::Create();
  }

  StorageInterface& Storage() {
    return *storage_;
  }

  /// Convenience: PutObject + Commit in one step.  Returns the new commit ID.
  std::string PutAndCommit(const std::string& branch, const std::string& path, const std::string& data, const std::string& msg = "test commit") {
    EXPECT_TRUE(storage_->PutObject(branch, path, data).ok());
    auto result = storage_->Commit(branch, msg);
    EXPECT_TRUE(result.ok()) << result.status();
    return *result;
  }

  std::unique_ptr<StorageInterface> storage_;
};

TYPED_TEST_SUITE_P(StorageConformanceTest);

// ===========================================================================
// Branch CRUD
// ===========================================================================

// 1. Canonical branch exists at construction; GetBranchHead returns a commit.
TYPED_TEST_P(StorageConformanceTest, CanonicalBranchExists) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  EXPECT_FALSE(canonical.empty());

  auto head = this->Storage().GetBranchHead(canonical);
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_FALSE(head->empty());
}

// 2. Create a branch forking from canonical head (empty base_commit_id).
TYPED_TEST_P(StorageConformanceTest, CreateBranchFromCanonical) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto canonical_head = this->Storage().GetBranchHead(canonical);
  ASSERT_TRUE(canonical_head.ok()) << canonical_head.status();

  auto branch = this->Storage().CreateBranch("feature-1", /*base_commit_id=*/"");
  ASSERT_TRUE(branch.ok()) << branch.status();
  EXPECT_EQ(*branch, "feature-1");

  auto branch_head = this->Storage().GetBranchHead("feature-1");
  ASSERT_TRUE(branch_head.ok()) << branch_head.status();
  EXPECT_EQ(*branch_head, *canonical_head);
}

// 3. Create a branch from a specific commit ID.
TYPED_TEST_P(StorageConformanceTest, CreateBranchFromCommit) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string commit_id = this->PutAndCommit(canonical, "file.txt", "data");

  auto branch = this->Storage().CreateBranch("from-commit", commit_id);
  ASSERT_TRUE(branch.ok()) << branch.status();

  auto head = this->Storage().GetBranchHead("from-commit");
  ASSERT_TRUE(head.ok()) << head.status();
  EXPECT_EQ(*head, commit_id);
}

// 4. Creating a branch with an existing name returns ALREADY_EXISTS.
TYPED_TEST_P(StorageConformanceTest, CreateBranchDuplicate) {
  auto first = this->Storage().CreateBranch("dup", "");
  ASSERT_TRUE(first.ok()) << first.status();

  auto second = this->Storage().CreateBranch("dup", "");
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.status().code(), absl::StatusCode::kAlreadyExists);
}

// 5. Delete a branch, verify it's gone.
TYPED_TEST_P(StorageConformanceTest, DeleteBranch) {
  auto create = this->Storage().CreateBranch("temp", "");
  ASSERT_TRUE(create.ok()) << create.status();

  auto del = this->Storage().DeleteBranch("temp");
  ASSERT_TRUE(del.ok()) << del;

  auto head = this->Storage().GetBranchHead("temp");
  ASSERT_FALSE(head.ok());
  EXPECT_EQ(head.status().code(), absl::StatusCode::kNotFound);
}

// 6. Deleting canonical branch returns FAILED_PRECONDITION.
TYPED_TEST_P(StorageConformanceTest, DeleteCanonicalBranch) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto del = this->Storage().DeleteBranch(canonical);
  ASSERT_FALSE(del.ok());
  EXPECT_EQ(del.code(), absl::StatusCode::kFailedPrecondition);
}

// 7. Deleting nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, DeleteNonexistentBranch) {
  auto del = this->Storage().DeleteBranch("no-such-branch");
  ASSERT_FALSE(del.ok());
  EXPECT_EQ(del.code(), absl::StatusCode::kNotFound);
}

// 8. Getting head of nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, GetBranchHeadNonexistent) {
  auto head = this->Storage().GetBranchHead("nonexistent");
  ASSERT_FALSE(head.ok());
  EXPECT_EQ(head.status().code(), absl::StatusCode::kNotFound);
}

// 9. Creating branch from nonexistent commit returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, CreateBranchFromNonexistentCommit) {
  auto branch = this->Storage().CreateBranch("bad-base", "nonexistent-commit-id");
  ASSERT_FALSE(branch.ok());
  EXPECT_EQ(branch.status().code(), absl::StatusCode::kNotFound);
}

// 10. Creating branch with a name that matches an existing commit ID returns
//     INVALID_ARGUMENT because commit-id namespace is reserved.
TYPED_TEST_P(StorageConformanceTest, CreateBranchNameCollidesWithCommitId) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string commit_id = this->PutAndCommit(canonical, "collision.txt", "data");

  auto branch = this->Storage().CreateBranch(commit_id, "");
  ASSERT_FALSE(branch.ok());
  EXPECT_EQ(branch.status().code(), absl::StatusCode::kInvalidArgument);
}

// 11. Creating a branch in the reserved commit-id namespace returns
//     INVALID_ARGUMENT even if no commit with that id exists yet.
TYPED_TEST_P(StorageConformanceTest, CreateBranchReservedCommitIdNamespace) {
  auto branch = this->Storage().CreateBranch("commit-999", "");
  ASSERT_FALSE(branch.ok());
  EXPECT_EQ(branch.status().code(), absl::StatusCode::kInvalidArgument);
}

// ===========================================================================
// Object I/O
// ===========================================================================

// 12. Put an object, get it back. Verify data matches.
TYPED_TEST_P(StorageConformanceTest, PutAndGetObject) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "hello.txt", "world").ok());

  auto data = this->Storage().GetObject(canonical, "hello.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "world");
}

// 11. Put object twice at same path; get returns latest data.
TYPED_TEST_P(StorageConformanceTest, PutOverwrite) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "key", "value-1").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "key", "value-2").ok());

  auto data = this->Storage().GetObject(canonical, "key");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "value-2");
}

// 12. Get object that doesn't exist returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, GetNonexistentObject) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto data = this->Storage().GetObject(canonical, "does/not/exist");
  ASSERT_FALSE(data.ok());
  EXPECT_EQ(data.status().code(), absl::StatusCode::kNotFound);
}

// 13. Put an object, commit, stage a delete. GetObject returns NOT_FOUND.
//     After commit, object is gone.
TYPED_TEST_P(StorageConformanceTest, DeleteObject) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "to-delete.txt", "content");

  auto del = this->Storage().DeleteObject(canonical, "to-delete.txt");
  ASSERT_TRUE(del.ok()) << del;

  // Staged delete hides the object.
  auto get_staged = this->Storage().GetObject(canonical, "to-delete.txt");
  ASSERT_FALSE(get_staged.ok());
  EXPECT_EQ(get_staged.status().code(), absl::StatusCode::kNotFound);

  // Commit the delete.
  auto commit = this->Storage().Commit(canonical, "delete obj");
  ASSERT_TRUE(commit.ok()) << commit.status();

  auto get_after = this->Storage().GetObject(canonical, "to-delete.txt");
  ASSERT_FALSE(get_after.ok());
  EXPECT_EQ(get_after.status().code(), absl::StatusCode::kNotFound);
}

// 14. Stage a delete for a committed object. Before and after commit,
//     GetObject returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, DeleteObjectTombstone) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "file.dat", "payload");

  ASSERT_TRUE(this->Storage().DeleteObject(canonical, "file.dat").ok());

  // Before commit — tombstone hides the committed object.
  auto before = this->Storage().GetObject(canonical, "file.dat");
  ASSERT_FALSE(before.ok());
  EXPECT_EQ(before.status().code(), absl::StatusCode::kNotFound);

  // After commit — permanently gone.
  ASSERT_TRUE(this->Storage().Commit(canonical, "tombstone commit").ok());

  auto after = this->Storage().GetObject(canonical, "file.dat");
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.status().code(), absl::StatusCode::kNotFound);
}

// 15. ObjectExists returns true for existing object, false for absent.
TYPED_TEST_P(StorageConformanceTest, ObjectExists) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "exists.txt", "yes").ok());

  auto exists = this->Storage().ObjectExists(canonical, "exists.txt");
  ASSERT_TRUE(exists.ok()) << exists.status();
  EXPECT_TRUE(*exists);

  auto missing = this->Storage().ObjectExists(canonical, "nope.txt");
  ASSERT_TRUE(missing.ok()) << missing.status();
  EXPECT_FALSE(*missing);
}

// 16. Put and commit, then stage delete. ObjectExists returns false.
TYPED_TEST_P(StorageConformanceTest, ObjectExistsWithTombstone) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "shadow.txt", "data");

  ASSERT_TRUE(this->Storage().DeleteObject(canonical, "shadow.txt").ok());

  auto exists = this->Storage().ObjectExists(canonical, "shadow.txt");
  ASSERT_TRUE(exists.ok()) << exists.status();
  EXPECT_FALSE(*exists);
}

// 17. ObjectExists accepts a commit ref and returns true for objects in that
//     commit, false for paths not in the commit.
TYPED_TEST_P(StorageConformanceTest, ObjectExistsOnCommitRef) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string commit_ref = this->PutAndCommit(canonical, "committed.txt", "data");

  auto yes = this->Storage().ObjectExists(commit_ref, "committed.txt");
  ASSERT_TRUE(yes.ok()) << yes.status();
  EXPECT_TRUE(*yes);

  auto no = this->Storage().ObjectExists(commit_ref, "other.txt");
  ASSERT_TRUE(no.ok()) << no.status();
  EXPECT_FALSE(*no);
}

// 18. ObjectExists with a nonexistent ref returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, ObjectExistsOnNonexistentRef) {
  auto result = this->Storage().ObjectExists("bad-commit", "any.txt");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

// 19. GetObject accepts a commit ref and returns correct data;
//     missing path -> NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, GetObjectOnCommitRef) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string commit_ref = this->PutAndCommit(canonical, "versioned.txt", "v1-data");

  auto data = this->Storage().GetObject(commit_ref, "versioned.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "v1-data");

  auto missing = this->Storage().GetObject(commit_ref, "absent.txt");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.status().code(), absl::StatusCode::kNotFound);
}

// 20. GetObject with nonexistent ref returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, GetObjectOnNonexistentRef) {
  auto data = this->Storage().GetObject("no-such-commit", "any.txt");
  ASSERT_FALSE(data.ok());
  EXPECT_EQ(data.status().code(), absl::StatusCode::kNotFound);
}

// 21. List objects on an empty branch returns empty list.
TYPED_TEST_P(StorageConformanceTest, ListObjectsEmpty) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto list = this->Storage().ListObjects(canonical, "");
  ASSERT_TRUE(list.ok()) << list.status();
  EXPECT_TRUE(list->empty());
}

// 22. List with prefix returns only matching objects, sorted.
TYPED_TEST_P(StorageConformanceTest, ListObjectsWithPrefix) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "dir/a.txt", "a").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "dir/b.txt", "b").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "other/c.txt", "c").ok());
  ASSERT_TRUE(this->Storage().Commit(canonical, "add files").ok());

  auto list = this->Storage().ListObjects(canonical, "dir/");
  ASSERT_TRUE(list.ok()) << list.status();
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ((*list)[0], "dir/a.txt");
  EXPECT_EQ((*list)[1], "dir/b.txt");

  // Full listing.
  auto all = this->Storage().ListObjects(canonical, "");
  ASSERT_TRUE(all.ok()) << all.status();
  ASSERT_EQ(all->size(), 3u);
  // Sorted order.
  EXPECT_TRUE(std::is_sorted(all->begin(), all->end()));
}

// 23. Staged puts are visible via GetObject before commit.
TYPED_TEST_P(StorageConformanceTest, StagedChangesVisibleInGet) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "staged.txt", "staged-data").ok());

  auto data = this->Storage().GetObject(canonical, "staged.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "staged-data");
}

// 24. Committed object hidden by staged delete.
TYPED_TEST_P(StorageConformanceTest, StagedDeleteHidesCommitted) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "visible.txt", "data");

  // Confirm the object is visible.
  auto before = this->Storage().GetObject(canonical, "visible.txt");
  ASSERT_TRUE(before.ok()) << before.status();
  EXPECT_EQ(*before, "data");

  // Stage a delete.
  ASSERT_TRUE(this->Storage().DeleteObject(canonical, "visible.txt").ok());

  // Now hidden.
  auto after = this->Storage().GetObject(canonical, "visible.txt");
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.status().code(), absl::StatusCode::kNotFound);
}

// 25. PutObject on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, PutOnNonexistentBranch) {
  auto result = this->Storage().PutObject("ghost-branch", "file.txt", "data");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.code(), absl::StatusCode::kNotFound);
}

// ===========================================================================
// Commit
// ===========================================================================

// 26. Commit advances the branch head.
TYPED_TEST_P(StorageConformanceTest, CommitCreatesNewHead) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto old_head = this->Storage().GetBranchHead(canonical);
  ASSERT_TRUE(old_head.ok()) << old_head.status();

  ASSERT_TRUE(this->Storage().PutObject(canonical, "new.txt", "data").ok());
  auto commit = this->Storage().Commit(canonical, "advance head");
  ASSERT_TRUE(commit.ok()) << commit.status();
  EXPECT_NE(*commit, *old_head);

  auto new_head = this->Storage().GetBranchHead(canonical);
  ASSERT_TRUE(new_head.ok()) << new_head.status();
  EXPECT_EQ(*new_head, *commit);
}

// 27. After commit, staging is clear — putting again and getting shows
//     no leftover staged data.
TYPED_TEST_P(StorageConformanceTest, CommitClearsStaging) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "staged-clear.txt", "original");

  // After commit, a new put should overwrite cleanly.
  ASSERT_TRUE(this->Storage().PutObject(canonical, "staged-clear.txt", "updated").ok());
  auto data = this->Storage().GetObject(canonical, "staged-clear.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "updated");
}

// 28. Commit with no staged changes succeeds (empty commit).
TYPED_TEST_P(StorageConformanceTest, EmptyCommit) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto commit = this->Storage().Commit(canonical, "empty");
  ASSERT_TRUE(commit.ok()) << commit.status();
  EXPECT_FALSE(commit->empty());
}

// 29. Commit object on branch, create new branch from that commit,
//     verify object exists there.
TYPED_TEST_P(StorageConformanceTest, CommitPreservesObjects) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string cid = this->PutAndCommit(canonical, "preserved.txt", "keep-me");

  auto branch = this->Storage().CreateBranch("fork", cid);
  ASSERT_TRUE(branch.ok()) << branch.status();

  auto data = this->Storage().GetObject("fork", "preserved.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "keep-me");
}

// 30. Commit on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, CommitOnNonexistentBranch) {
  auto commit = this->Storage().Commit("no-branch", "msg");
  ASSERT_FALSE(commit.ok());
  EXPECT_EQ(commit.status().code(), absl::StatusCode::kNotFound);
}

// ===========================================================================
// Merge — No Conflict
// ===========================================================================

// 31. Create branch from canonical, add objects, commit.
//     Merge branch into canonical. Canonical now has the objects.
TYPED_TEST_P(StorageConformanceTest, MergeNoConflict) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("feature", "").ok());

  this->PutAndCommit("feature", "feature.txt", "feature-data");

  auto merge = this->Storage().Merge("feature", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto data = this->Storage().GetObject(canonical, "feature.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "feature-data");
}

// 32. Both branches add different paths. Merge succeeds with all objects.
TYPED_TEST_P(StorageConformanceTest, MergeNoConflictBothSidesAddDifferentPaths) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("b1", "").ok());

  this->PutAndCommit(canonical, "canon.txt", "canon-data");
  this->PutAndCommit("b1", "b1.txt", "b1-data");

  auto merge = this->Storage().Merge("b1", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto d1 = this->Storage().GetObject(canonical, "canon.txt");
  ASSERT_TRUE(d1.ok()) << d1.status();
  EXPECT_EQ(*d1, "canon-data");

  auto d2 = this->Storage().GetObject(canonical, "b1.txt");
  ASSERT_TRUE(d2.ok()) << d2.status();
  EXPECT_EQ(*d2, "b1-data");
}

// 33. Both branches make identical changes. Merge succeeds.
TYPED_TEST_P(StorageConformanceTest, MergeIdenticalChanges) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("ident", "").ok());

  this->PutAndCommit(canonical, "same.txt", "same-value");
  this->PutAndCommit("ident", "same.txt", "same-value");

  auto merge = this->Storage().Merge("ident", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto data = this->Storage().GetObject(canonical, "same.txt");
  ASSERT_TRUE(data.ok()) << data.status();
  EXPECT_EQ(*data, "same-value");
}

// 34. One branch deletes a path, other doesn't touch it. Merge succeeds
//     with path deleted.
TYPED_TEST_P(StorageConformanceTest, MergeDeletionOnOneSide) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "doomed.txt", "soon-gone");

  ASSERT_TRUE(this->Storage().CreateBranch("deleter", "").ok());

  // Delete on the branch.
  ASSERT_TRUE(this->Storage().DeleteObject("deleter", "doomed.txt").ok());
  ASSERT_TRUE(this->Storage().Commit("deleter", "delete doomed").ok());

  auto merge = this->Storage().Merge("deleter", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto exists = this->Storage().ObjectExists(canonical, "doomed.txt");
  ASSERT_TRUE(exists.ok()) << exists.status();
  EXPECT_FALSE(*exists);
}

// 35. Merge when source and target share the same head (no-op).
TYPED_TEST_P(StorageConformanceTest, MergeSourceAndTargetSameCommit) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("same-head", "").ok());

  // Both branches point to the same commit — no changes on either side.
  auto merge = this->Storage().Merge("same-head", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());
}

// ===========================================================================
// Merge — Conflict
// ===========================================================================

// 36. Both branches modify the same path differently. Conflict.
TYPED_TEST_P(StorageConformanceTest, MergeConflict) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "shared.txt", "base");

  ASSERT_TRUE(this->Storage().CreateBranch("conflict-src", "").ok());

  this->PutAndCommit(canonical, "shared.txt", "canon-edit");
  this->PutAndCommit("conflict-src", "shared.txt", "src-edit");

  auto merge = this->Storage().Merge("conflict-src", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsConflict());

  const auto& conflict = merge->GetConflict();
  ASSERT_EQ(conflict.conflicting_paths.size(), 1u);
  EXPECT_EQ(conflict.conflicting_paths[0], "shared.txt");
}

// 37. One branch deletes, other modifies. Conflict.
TYPED_TEST_P(StorageConformanceTest, MergeConflictOneDeleteOneModify) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "clash.txt", "original");

  ASSERT_TRUE(this->Storage().CreateBranch("modifier", "").ok());

  // Canonical deletes.
  ASSERT_TRUE(this->Storage().DeleteObject(canonical, "clash.txt").ok());
  ASSERT_TRUE(this->Storage().Commit(canonical, "delete clash").ok());

  // Branch modifies.
  this->PutAndCommit("modifier", "clash.txt", "modified");

  auto merge = this->Storage().Merge("modifier", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsConflict());

  const auto& conflict = merge->GetConflict();
  ASSERT_EQ(conflict.conflicting_paths.size(), 1u);
  EXPECT_EQ(conflict.conflicting_paths[0], "clash.txt");
}

// 38. Multiple conflicting paths returned in sorted order.
TYPED_TEST_P(StorageConformanceTest, MergeConflictMultiplePaths) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  // Create base state with two files.
  ASSERT_TRUE(this->Storage().PutObject(canonical, "z.txt", "base").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "a.txt", "base").ok());
  ASSERT_TRUE(this->Storage().Commit(canonical, "base state").ok());

  ASSERT_TRUE(this->Storage().CreateBranch("multi-conflict", "").ok());

  // Both sides edit both files differently.
  ASSERT_TRUE(this->Storage().PutObject(canonical, "a.txt", "canon-a").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "z.txt", "canon-z").ok());
  ASSERT_TRUE(this->Storage().Commit(canonical, "canon edits").ok());

  ASSERT_TRUE(this->Storage().PutObject("multi-conflict", "a.txt", "branch-a").ok());
  ASSERT_TRUE(this->Storage().PutObject("multi-conflict", "z.txt", "branch-z").ok());
  ASSERT_TRUE(this->Storage().Commit("multi-conflict", "branch edits").ok());

  auto merge = this->Storage().Merge("multi-conflict", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsConflict());

  const auto& conflict = merge->GetConflict();
  ASSERT_GE(conflict.conflicting_paths.size(), 2u);
  EXPECT_TRUE(std::is_sorted(conflict.conflicting_paths.begin(), conflict.conflicting_paths.end()));
  EXPECT_EQ(conflict.conflicting_paths[0], "a.txt");
  EXPECT_EQ(conflict.conflicting_paths[1], "z.txt");
}

// 39. Conflict result includes correct commit IDs.
TYPED_TEST_P(StorageConformanceTest, MergeConflictIncludesBaseAndHeadCommits) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string base_cid = this->PutAndCommit(canonical, "info.txt", "base");

  ASSERT_TRUE(this->Storage().CreateBranch("info-branch", "").ok());

  std::string canon_cid = this->PutAndCommit(canonical, "info.txt", "canon-v2");
  std::string branch_cid = this->PutAndCommit("info-branch", "info.txt", "branch-v2");

  auto merge = this->Storage().Merge("info-branch", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsConflict());

  const auto& conflict = merge->GetConflict();
  EXPECT_EQ(conflict.base_commit_id, base_cid);
  EXPECT_EQ(conflict.source_commit_id, branch_cid);
  EXPECT_EQ(conflict.target_commit_id, canon_cid);
}

// ===========================================================================
// Merge — Preconditions
// ===========================================================================

// 40. Merge fails with FAILED_PRECONDITION if source has uncommitted changes.
TYPED_TEST_P(StorageConformanceTest, MergeWithUncommittedChangesSource) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("dirty-src", "").ok());

  ASSERT_TRUE(this->Storage().PutObject("dirty-src", "uncommitted.txt", "data").ok());

  auto merge = this->Storage().Merge("dirty-src", canonical);
  ASSERT_FALSE(merge.ok());
  EXPECT_EQ(merge.status().code(), absl::StatusCode::kFailedPrecondition);
}

// 41. Merge fails with FAILED_PRECONDITION if target has uncommitted changes.
TYPED_TEST_P(StorageConformanceTest, MergeWithUncommittedChangesTarget) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("clean-src", "").ok());

  // Dirty the target (canonical).
  ASSERT_TRUE(this->Storage().PutObject(canonical, "uncommitted.txt", "data").ok());

  auto merge = this->Storage().Merge("clean-src", canonical);
  ASSERT_FALSE(merge.ok());
  EXPECT_EQ(merge.status().code(), absl::StatusCode::kFailedPrecondition);
}

// 42. Merge from nonexistent source returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, MergeNonexistentSourceBranch) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto merge = this->Storage().Merge("ghost", canonical);
  ASSERT_FALSE(merge.ok());
  EXPECT_EQ(merge.status().code(), absl::StatusCode::kNotFound);
}

// 43. Merge into nonexistent target returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, MergeNonexistentTargetBranch) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto merge = this->Storage().Merge(canonical, "phantom");
  ASSERT_FALSE(merge.ok());
  EXPECT_EQ(merge.status().code(), absl::StatusCode::kNotFound);
}

// 44. Merging a branch into itself returns INVALID_ARGUMENT.
TYPED_TEST_P(StorageConformanceTest, MergeSameSourceAndTargetBranch) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto merge = this->Storage().Merge(canonical, canonical);
  ASSERT_FALSE(merge.ok());
  EXPECT_EQ(merge.status().code(), absl::StatusCode::kInvalidArgument);
}

// ===========================================================================
// Merge — Complex
// ===========================================================================

// 45. Chained branches: A from canonical, B from A. Merge B → canonical.
TYPED_TEST_P(StorageConformanceTest, MergeChainedBranches) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("chain-A", "").ok());
  std::string a_cid = this->PutAndCommit("chain-A", "a-file.txt", "from-A");

  auto branch_b = this->Storage().CreateBranch("chain-B", a_cid);
  ASSERT_TRUE(branch_b.ok()) << branch_b.status();
  this->PutAndCommit("chain-B", "b-file.txt", "from-B");

  // Merge chain-B into canonical (which brings both A and B changes).
  auto merge = this->Storage().Merge("chain-B", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto a_data = this->Storage().GetObject(canonical, "a-file.txt");
  ASSERT_TRUE(a_data.ok()) << a_data.status();
  EXPECT_EQ(*a_data, "from-A");

  auto b_data = this->Storage().GetObject(canonical, "b-file.txt");
  ASSERT_TRUE(b_data.ok()) << b_data.status();
  EXPECT_EQ(*b_data, "from-B");
}

// 45. Merge two branches sequentially into canonical.
TYPED_TEST_P(StorageConformanceTest, MergeTwoBranchesSequentially) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("seq-1", "").ok());
  ASSERT_TRUE(this->Storage().CreateBranch("seq-2", "").ok());

  this->PutAndCommit("seq-1", "seq1.txt", "seq1-data");
  this->PutAndCommit("seq-2", "seq2.txt", "seq2-data");

  auto merge1 = this->Storage().Merge("seq-1", canonical);
  ASSERT_TRUE(merge1.ok()) << merge1.status();
  ASSERT_TRUE(merge1->IsSuccess());

  auto merge2 = this->Storage().Merge("seq-2", canonical);
  ASSERT_TRUE(merge2.ok()) << merge2.status();
  ASSERT_TRUE(merge2->IsSuccess());

  auto d1 = this->Storage().GetObject(canonical, "seq1.txt");
  ASSERT_TRUE(d1.ok()) << d1.status();
  EXPECT_EQ(*d1, "seq1-data");

  auto d2 = this->Storage().GetObject(canonical, "seq2.txt");
  ASSERT_TRUE(d2.ok()) << d2.status();
  EXPECT_EQ(*d2, "seq2-data");
}

// 46. Diamond history: A and B branch from canonical. Merge A into canonical.
//     Then merge B into canonical (merge base is original canonical head).
TYPED_TEST_P(StorageConformanceTest, MergeWithDiamondHistory) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  auto original_head = this->Storage().GetBranchHead(canonical);
  ASSERT_TRUE(original_head.ok()) << original_head.status();

  ASSERT_TRUE(this->Storage().CreateBranch("diamond-A", "").ok());
  ASSERT_TRUE(this->Storage().CreateBranch("diamond-B", "").ok());

  this->PutAndCommit("diamond-A", "a-only.txt", "A-data");
  this->PutAndCommit("diamond-B", "b-only.txt", "B-data");

  // Merge A first.
  auto merge_a = this->Storage().Merge("diamond-A", canonical);
  ASSERT_TRUE(merge_a.ok()) << merge_a.status();
  ASSERT_TRUE(merge_a->IsSuccess());

  // Merge B second — should use original canonical head as merge base,
  // not the merge commit from A.
  auto merge_b = this->Storage().Merge("diamond-B", canonical);
  ASSERT_TRUE(merge_b.ok()) << merge_b.status();
  ASSERT_TRUE(merge_b->IsSuccess());

  // Both sets of changes present.
  auto a_data = this->Storage().GetObject(canonical, "a-only.txt");
  ASSERT_TRUE(a_data.ok()) << a_data.status();
  EXPECT_EQ(*a_data, "A-data");

  auto b_data = this->Storage().GetObject(canonical, "b-only.txt");
  ASSERT_TRUE(b_data.ok()) << b_data.status();
  EXPECT_EQ(*b_data, "B-data");
}

// ===========================================================================
// Cross-cutting
// ===========================================================================

// 47. Changes on one branch are not visible on another until merge.
TYPED_TEST_P(StorageConformanceTest, BranchIsolation) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("isolated", "").ok());

  this->PutAndCommit("isolated", "secret.txt", "hidden");

  // Canonical should not see the object.
  auto exists = this->Storage().ObjectExists(canonical, "secret.txt");
  ASSERT_TRUE(exists.ok()) << exists.status();
  EXPECT_FALSE(*exists);

  auto get = this->Storage().GetObject(canonical, "secret.txt");
  ASSERT_FALSE(get.ok());
  EXPECT_EQ(get.status().code(), absl::StatusCode::kNotFound);
}

// 48. After committing, the commit's objects don't change when the branch
//     advances.
TYPED_TEST_P(StorageConformanceTest, CommitImmutability) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  std::string commit_ref = this->PutAndCommit(canonical, "immutable.txt", "v1");

  // Advance the branch.
  this->PutAndCommit(canonical, "immutable.txt", "v2");

  // The old commit should still return v1.
  auto old_data = this->Storage().GetObject(commit_ref, "immutable.txt");
  ASSERT_TRUE(old_data.ok()) << old_data.status();
  EXPECT_EQ(*old_data, "v1");

  // Current branch returns v2.
  auto cur_data = this->Storage().GetObject(canonical, "immutable.txt");
  ASSERT_TRUE(cur_data.ok()) << cur_data.status();
  EXPECT_EQ(*cur_data, "v2");
}

// ===========================================================================
// Additional edge cases (from review)
// ===========================================================================

// 49. ListObjects sees staged (uncommitted) objects.
TYPED_TEST_P(StorageConformanceTest, ListObjectsIncludesStaged) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().PutObject(canonical, "staged/a.txt", "a").ok());
  ASSERT_TRUE(this->Storage().PutObject(canonical, "staged/b.txt", "b").ok());

  auto list = this->Storage().ListObjects(canonical, "staged/");
  ASSERT_TRUE(list.ok()) << list.status();
  ASSERT_EQ(list->size(), 2u);
  EXPECT_EQ((*list)[0], "staged/a.txt");
  EXPECT_EQ((*list)[1], "staged/b.txt");
}

// 50. ListObjects excludes tombstoned objects.
TYPED_TEST_P(StorageConformanceTest, ListObjectsExcludesTombstoned) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  this->PutAndCommit(canonical, "alive.txt", "data");
  this->PutAndCommit(canonical, "dead.txt", "data");

  // Stage a delete for "dead.txt".
  ASSERT_TRUE(this->Storage().DeleteObject(canonical, "dead.txt").ok());

  auto list = this->Storage().ListObjects(canonical, "");
  ASSERT_TRUE(list.ok()) << list.status();
  ASSERT_EQ(list->size(), 1u);
  EXPECT_EQ((*list)[0], "alive.txt");
}

// 51. DeleteObject on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, DeleteObjectOnNonexistentBranch) {
  auto result = this->Storage().DeleteObject("ghost-branch", "file.txt");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.code(), absl::StatusCode::kNotFound);
}

// 52. GetObject on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, GetObjectOnNonexistentBranch) {
  auto result = this->Storage().GetObject("ghost-branch", "file.txt");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

// 53. ObjectExists on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, ObjectExistsOnNonexistentBranch) {
  auto result = this->Storage().ObjectExists("ghost-branch", "file.txt");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

// 54. ListObjects on nonexistent branch returns NOT_FOUND.
TYPED_TEST_P(StorageConformanceTest, ListObjectsOnNonexistentBranch) {
  auto result = this->Storage().ListObjects("ghost-branch", "");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), absl::StatusCode::kNotFound);
}

// 55. Merge does not advance source branch head.
TYPED_TEST_P(StorageConformanceTest, MergeDoesNotAdvanceSourceHead) {
  const std::string canonical = this->Storage().GetCanonicalBranch();
  ASSERT_TRUE(this->Storage().CreateBranch("src-branch", "").ok());
  this->PutAndCommit("src-branch", "data.txt", "content");

  auto src_head_before = this->Storage().GetBranchHead("src-branch");
  ASSERT_TRUE(src_head_before.ok()) << src_head_before.status();

  auto merge = this->Storage().Merge("src-branch", canonical);
  ASSERT_TRUE(merge.ok()) << merge.status();
  ASSERT_TRUE(merge->IsSuccess());

  auto src_head_after = this->Storage().GetBranchHead("src-branch");
  ASSERT_TRUE(src_head_after.ok()) << src_head_after.status();
  EXPECT_EQ(*src_head_before, *src_head_after);
}

// ===========================================================================
// Register all test cases
// ===========================================================================

REGISTER_TYPED_TEST_SUITE_P(StorageConformanceTest,
                            // Branch CRUD
                            CanonicalBranchExists, CreateBranchFromCanonical, CreateBranchFromCommit, CreateBranchDuplicate, DeleteBranch,
                            DeleteCanonicalBranch, DeleteNonexistentBranch, GetBranchHeadNonexistent, CreateBranchFromNonexistentCommit,
                            CreateBranchNameCollidesWithCommitId, CreateBranchReservedCommitIdNamespace,
                            // Object I/O
                            PutAndGetObject, PutOverwrite, GetNonexistentObject, DeleteObject, DeleteObjectTombstone, ObjectExists, ObjectExistsWithTombstone,
                            ObjectExistsOnCommitRef, ObjectExistsOnNonexistentRef, GetObjectOnCommitRef, GetObjectOnNonexistentRef, ListObjectsEmpty,
                            ListObjectsWithPrefix, StagedChangesVisibleInGet, StagedDeleteHidesCommitted, PutOnNonexistentBranch, ListObjectsIncludesStaged,
                            ListObjectsExcludesTombstoned, DeleteObjectOnNonexistentBranch, GetObjectOnNonexistentBranch, ObjectExistsOnNonexistentBranch,
                            ListObjectsOnNonexistentBranch,
                            // Commit
                            CommitCreatesNewHead, CommitClearsStaging, EmptyCommit, CommitPreservesObjects, CommitOnNonexistentBranch,
                            // Merge — No Conflict
                            MergeNoConflict, MergeNoConflictBothSidesAddDifferentPaths, MergeIdenticalChanges, MergeDeletionOnOneSide,
                            MergeSourceAndTargetSameCommit,
                            // Merge — Conflict
                            MergeConflict, MergeConflictOneDeleteOneModify, MergeConflictMultiplePaths, MergeConflictIncludesBaseAndHeadCommits,
                            // Merge — Preconditions
                            MergeWithUncommittedChangesSource, MergeWithUncommittedChangesTarget, MergeNonexistentSourceBranch, MergeNonexistentTargetBranch,
                            MergeSameSourceAndTargetBranch,
                            // Merge — Complex
                            MergeChainedBranches, MergeTwoBranchesSequentially, MergeWithDiamondHistory,
                            // Merge — Additional
                            MergeDoesNotAdvanceSourceHead,
                            // Cross-cutting
                            BranchIsolation, CommitImmutability);

} // namespace artifact_system::testing

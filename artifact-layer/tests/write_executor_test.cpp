#include "transaction/write_executor.h"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::transaction::PathConflictKind;
using artifact_system::transaction::WriteExecutor;
using artifact_system::transaction::WriteExecutorOptions;
using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::StrictMock;

class MockStorage : public StorageInterface {
public:
  MOCK_METHOD(absl::StatusOr<std::string>, CreateBranch, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::Status, DeleteBranch, (const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, GetBranchHead, (const std::string&), (override));
  MOCK_METHOD(absl::Status, PutObject, (const std::string&, const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, GetObject, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::Status, DeleteObject, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<bool>, ObjectExists, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<std::vector<std::string>>, ListObjects, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<std::string>, Commit, (const std::string&, const std::string&), (override));
  MOCK_METHOD(absl::StatusOr<MergeResult>, Merge, (const std::string&, const std::string&), (override));
  MOCK_METHOD(std::string, GetCanonicalBranch, (), (const, override));
};

MergeResult BuildMergeSuccess(const std::string& commit_id) {
  MergeResult result;
  result.result = MergeResult::Success{.commit_id = commit_id};
  return result;
}

MergeResult BuildMergeConflict(std::initializer_list<std::string> paths) {
  MergeResult result;
  MergeResult::Conflict conflict;
  for (const std::string& path : paths) {
    conflict.conflicting_paths.push_back(path);
  }
  conflict.base_commit_id = "base";
  conflict.source_commit_id = "source";
  conflict.target_commit_id = "target";
  result.result = std::move(conflict);
  return result;
}

TEST(WriteExecutorTest, SuccessfulWriteUpdatesTransactionBranchHead) {
  StrictMock<MockStorage> storage;
  WriteExecutor executor(&storage);

  std::string child_branch;
  std::string transaction_head = "tx-head-1";

  InSequence seq;
  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>(transaction_head)));
  EXPECT_CALL(storage, CreateBranch(_, "tx-head-1")).WillOnce(DoAll(SaveArg<0>(&child_branch), Invoke([](const std::string& name, const std::string&) {
                                                                      return absl::StatusOr<std::string>(name);
                                                                    })));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Invoke([&](const std::string& branch, const std::string&) {
    EXPECT_EQ(branch, child_branch);
    return absl::StatusOr<std::string>("child-commit-1");
  }));
  EXPECT_CALL(storage, Merge(_, "txn-1")).WillOnce(Invoke([&](const std::string& source, const std::string&) {
    EXPECT_EQ(source, child_branch);
    transaction_head = "merged-1";
    return absl::StatusOr<MergeResult>(BuildMergeSuccess("merged-1"));
  }));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Invoke([&](const std::string& branch) {
    EXPECT_EQ(branch, child_branch);
    return absl::OkStatus();
  }));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteSuccess>(*result_or));
  const auto& success = std::get<WriteExecutor::WriteSuccess>(*result_or);
  EXPECT_EQ(success.commit_id, "merged-1");
  EXPECT_EQ(success.attempts, 1);
  EXPECT_EQ(transaction_head, "merged-1");
}

TEST(WriteExecutorTest, StagingFailurePropagatesAndCleansChildBranch) {
  StrictMock<MockStorage> storage;
  WriteExecutor executor(&storage);

  std::string child_branch;

  InSequence seq;
  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head-1")));
  EXPECT_CALL(storage, CreateBranch(_, "tx-head-1")).WillOnce(DoAll(SaveArg<0>(&child_branch), Invoke([](const std::string& name, const std::string&) {
                                                                      return absl::StatusOr<std::string>(name);
                                                                    })));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Invoke([&](const std::string& branch) {
    EXPECT_EQ(branch, child_branch);
    return absl::OkStatus();
  }));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::InvalidArgumentError("staging failed"); });
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(WriteExecutorTest, NonRetryableConflictReturnsImmediately) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/unique/", 0) == 0) {
      return PathConflictKind::kNonRetryableUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  options.sleep_for = [](absl::Duration) {};
  WriteExecutor executor(&storage, options);

  std::string child_branch;

  InSequence seq;
  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head-1")));
  EXPECT_CALL(storage, CreateBranch(_, "tx-head-1")).WillOnce(DoAll(SaveArg<0>(&child_branch), Invoke([](const std::string& name, const std::string&) {
                                                                      return absl::StatusOr<std::string>(name);
                                                                    })));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit-1")));
  EXPECT_CALL(storage, Merge(_, "txn-1")).WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeConflict({"idx/unique/abc"}))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::OkStatus()));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteConflict>(*result_or));

  const auto& conflict = std::get<WriteExecutor::WriteConflict>(*result_or);
  EXPECT_EQ(conflict.attempts, 1);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.attempts(), 1);
}

TEST(WriteExecutorTest, RetryableConflictRetriesThenSucceeds) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };

  std::vector<absl::Duration> sleeps;
  options.sleep_for = [&](absl::Duration d) { sleeps.push_back(d); };
  WriteExecutor executor(&storage, options);

  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head-1")));

  EXPECT_CALL(storage, CreateBranch(_, _)).WillOnce(Invoke([](const std::string& name, const std::string&) { return absl::StatusOr<std::string>(name); }));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit")));
  EXPECT_CALL(storage, Merge(_, "txn-1"))
      .WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeConflict({"idx/non_unique/abc"}))))
      .WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeSuccess("merged-2"))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::OkStatus()));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteSuccess>(*result_or));

  const auto& success = std::get<WriteExecutor::WriteSuccess>(*result_or);
  EXPECT_EQ(success.commit_id, "merged-2");
  EXPECT_EQ(success.attempts, 2);
  ASSERT_EQ(sleeps.size(), 1);
  EXPECT_GT(sleeps[0], absl::ZeroDuration());
}

TEST(WriteExecutorTest, RetryExhaustionReturnsNonRetryableConflict) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  options.conflict_options.max_attempts = 3;
  options.conflict_options.initial_backoff = absl::ZeroDuration();
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };

  uint32_t sleep_calls = 0;
  options.sleep_for = [&](absl::Duration) { ++sleep_calls; };
  WriteExecutor executor(&storage, options);

  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head")));
  EXPECT_CALL(storage, CreateBranch(_, _)).WillOnce(Invoke([](const std::string& name, const std::string&) { return absl::StatusOr<std::string>(name); }));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit")));
  EXPECT_CALL(storage, Merge(_, "txn-1")).Times(3).WillRepeatedly(Return(absl::StatusOr<MergeResult>(BuildMergeConflict({"idx/non_unique/abc"}))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::OkStatus()));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteConflict>(*result_or));

  const auto& conflict = std::get<WriteExecutor::WriteConflict>(*result_or);
  EXPECT_EQ(conflict.attempts, 3);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.detail.attempts(), 3);
  EXPECT_EQ(sleep_calls, 2);
}

TEST(WriteExecutorTest, RetryableConflictCanBeResolvedByCallback) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  uint32_t resolver_calls = 0;
  options.retry_conflict_resolver = [&](const artifact_system::transaction::RetryResolutionContext& context) {
    ++resolver_calls;
    EXPECT_EQ(context.source_branch.rfind("txn-1.write-", 0), 0U);
    EXPECT_EQ(context.target_branch, "txn-1");
    EXPECT_EQ(context.attempts_performed, 1);
    return absl::StatusOr<bool>(true);
  };
  options.sleep_for = [](absl::Duration) {};
  WriteExecutor executor(&storage, options);

  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head-1")));
  EXPECT_CALL(storage, CreateBranch(_, _)).WillOnce(Invoke([](const std::string& name, const std::string&) { return absl::StatusOr<std::string>(name); }));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit")));
  EXPECT_CALL(storage, Merge(_, "txn-1"))
      .WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeConflict({"idx/non_unique/abc"}))))
      .WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeSuccess("merged-2"))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::OkStatus()));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteSuccess>(*result_or));
  EXPECT_EQ(resolver_calls, 1);
}

TEST(WriteExecutorTest, ResolverCanAbortRetryableConflict) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  options.path_conflict_classifier = [](const std::string& path) {
    if (path.rfind("idx/non_unique/", 0) == 0) {
      return PathConflictKind::kRetryableNonUniqueIndex;
    }
    return PathConflictKind::kNonRetryableUnknown;
  };
  options.retry_conflict_resolver = [](const artifact_system::transaction::RetryResolutionContext&) { return absl::StatusOr<bool>(false); };
  options.sleep_for = [](absl::Duration) {};
  WriteExecutor executor(&storage, options);

  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head")));
  EXPECT_CALL(storage, CreateBranch(_, _)).WillOnce(Invoke([](const std::string& name, const std::string&) { return absl::StatusOr<std::string>(name); }));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit")));
  EXPECT_CALL(storage, Merge(_, "txn-1")).WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeConflict({"idx/non_unique/abc"}))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::OkStatus()));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteConflict>(*result_or));
  const auto& conflict = std::get<WriteExecutor::WriteConflict>(*result_or);
  EXPECT_FALSE(conflict.detail.retryable());
  EXPECT_EQ(conflict.attempts, 1);
}

TEST(WriteExecutorTest, CleanupFailureInvokesErrorHandler) {
  StrictMock<MockStorage> storage;
  WriteExecutorOptions options;
  bool cleanup_failure_seen = false;
  options.on_cleanup_failure = [&](const absl::Status& status) {
    cleanup_failure_seen = true;
    EXPECT_EQ(status.code(), absl::StatusCode::kInternal);
  };
  WriteExecutor executor(&storage, options);

  InSequence seq;
  EXPECT_CALL(storage, GetBranchHead("txn-1")).WillOnce(Return(absl::StatusOr<std::string>("tx-head-1")));
  EXPECT_CALL(storage, CreateBranch(_, "tx-head-1")).WillOnce(Invoke([](const std::string& name, const std::string&) {
    return absl::StatusOr<std::string>(name);
  }));
  EXPECT_CALL(storage, Commit(_, _)).WillOnce(Return(absl::StatusOr<std::string>("child-commit-1")));
  EXPECT_CALL(storage, Merge(_, "txn-1")).WillOnce(Return(absl::StatusOr<MergeResult>(BuildMergeSuccess("merged-1"))));
  EXPECT_CALL(storage, DeleteBranch(_)).WillOnce(Return(absl::InternalError("cleanup failed")));

  auto result_or = executor.ExecuteWrite("txn-1", [](const std::string&) { return absl::OkStatus(); });
  ASSERT_TRUE(result_or.ok());
  ASSERT_TRUE(std::holds_alternative<WriteExecutor::WriteSuccess>(*result_or));
  EXPECT_TRUE(cleanup_failure_seen);
}

} // namespace
} // namespace artifact_system::testing

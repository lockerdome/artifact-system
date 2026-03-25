#include "storage/memory_storage.h"
#include "storage_conformance_test.h"

#include "gtest/gtest.h"

namespace artifact_system::testing {

// Factory for MemoryStorage.
struct MemoryStorageFactory {
  static std::unique_ptr<StorageInterface> Create() {
    return std::make_unique<MemoryStorage>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(MemoryStorage, StorageConformanceTest, MemoryStorageFactory);

TEST(MemoryStorageBehaviorTest, CreateBranchReservedCommitIdNamespace) {
  MemoryStorage storage;

  auto branch = storage.CreateBranch("commit-999", "");
  ASSERT_FALSE(branch.ok());
  EXPECT_EQ(branch.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace artifact_system::testing

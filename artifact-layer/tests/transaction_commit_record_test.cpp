#include "bootstrap/genesis.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "id/id_allocator_interface.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "index/index_utils.h"
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::bootstrap::GenesisIds;
using artifact_system::bootstrap::GenesisResult;
using artifact_system::transaction::TransactionManager;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

IndexDefinition FindIndexDef(const google::protobuf::Descriptor& desc, const std::string& key_type) {
  auto result = index::FindIndexDefinition(desc, key_type);
  EXPECT_TRUE(result.has_value()) << "index def not found: " << key_type;
  return result.value_or(IndexDefinition{});
}

index::IndexObject ReadIndexObject(MemoryStorage& storage, const std::string& branch, uint64_t index_def_id, std::span<const uint8_t> encoded_key,
                                   const IndexDefinition& idx_def, const google::protobuf::Descriptor& parent_desc) {
  std::string path = encoding::IndexPath(index_def_id, encoded_key);
  auto data = storage.GetObject(branch, path);
  EXPECT_TRUE(data.ok()) << "index " << index_def_id << ": " << data.status();
  auto schema = index::GenerateIndexSchema(idx_def, parent_desc);
  EXPECT_TRUE(schema.ok()) << schema.status();
  auto obj = index::DeserializeIndexObject(*schema, idx_def, *data);
  EXPECT_TRUE(obj.ok()) << obj.status();
  return *obj;
}

bool IndexEntryExists(MemoryStorage& storage, const std::string& branch, uint64_t index_def_id, std::span<const uint8_t> encoded_key) {
  std::string path = encoding::IndexPath(index_def_id, encoded_key);
  auto data = storage.GetObject(branch, path);
  return data.ok();
}

TransactionManager::CommitRecordConfig MakeCommitRecordConfig(const GenesisResult& genesis_result, IdAllocatorInterface* id_alloc) {
  TransactionManager::CommitRecordConfig cfg;
  cfg.index_def_id = GenesisIds::kTransactionCommitById;
  cfg.version_def_id = GenesisIds::kTransactionCommitRecordTypeVersionDef;
  cfg.index_def_ids_by_key_type = {{"transaction_commit_by_id", GenesisIds::kTransactionCommitById}};
  cfg.id_allocator = id_alloc;
  return cfg;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class TransactionCommitRecordTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = bootstrap::RunGenesis(&storage_);
    ASSERT_TRUE(result.ok()) << result.status();
    genesis_result_ = std::move(*result);

    TransactionManager::Options opts;
    opts.commit_record_config = MakeCommitRecordConfig(genesis_result_, &id_alloc_);
    manager_ = std::make_unique<TransactionManager>(&storage_, opts);
  }

  MemoryStorage storage_;
  GenesisResult genesis_result_;
  MockIdAllocator id_alloc_{GenesisIds::kFirstUserAllocatableId};
  std::unique_ptr<TransactionManager> manager_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(TransactionCommitRecordTest, TopLevelCommitCreatesRecord) {
  auto txn_or = manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& txn_id = *txn_or;

  // Write something so the transaction is non-empty.
  // Tests write directly to storage and commit; production code uses WriteExecutor.
  ASSERT_TRUE(storage_.PutObject(txn_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(txn_id, "test write").ok());

  auto commit_or = manager_->CommitTransaction(txn_id);
  ASSERT_TRUE(commit_or.ok()) << commit_or.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_or));

  // Verify the record exists on the canonical branch via index lookup.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", txn_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  std::string canonical = storage_.GetCanonicalBranch();
  EXPECT_TRUE(IndexEntryExists(storage_, canonical, GenesisIds::kTransactionCommitById, *encoded_key));
}

TEST_F(TransactionCommitRecordTest, SubTransactionCommitCreatesRecordOnParentBranch) {
  // Create parent transaction.
  auto parent_or = manager_->CreateTransaction();
  ASSERT_TRUE(parent_or.ok()) << parent_or.status();
  const std::string& parent_id = *parent_or;

  // Create sub-transaction under the parent.
  auto child_or = manager_->CreateTransaction(/*parent_snapshot_id=*/std::nullopt, /*parent_transaction_id=*/parent_id);
  ASSERT_TRUE(child_or.ok()) << child_or.status();
  const std::string& child_id = *child_or;

  ASSERT_TRUE(storage_.PutObject(child_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(child_id, "test write").ok());

  auto commit_or = manager_->CommitTransaction(child_id);
  ASSERT_TRUE(commit_or.ok()) << commit_or.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_or));

  // Verify the record exists on the parent transaction branch.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", child_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  EXPECT_TRUE(IndexEntryExists(storage_, parent_id, GenesisIds::kTransactionCommitById, *encoded_key));

  // Clean up: commit parent.
  auto parent_commit = manager_->CommitTransaction(parent_id);
  ASSERT_TRUE(parent_commit.ok()) << parent_commit.status();
}

TEST_F(TransactionCommitRecordTest, RecordContainsCorrectFields) {
  int64_t before = absl::ToUnixSeconds(absl::Now());

  auto txn_or = manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& txn_id = *txn_or;

  ASSERT_TRUE(storage_.PutObject(txn_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(txn_id, "test write").ok());

  auto commit_or = manager_->CommitTransaction(txn_id);
  ASSERT_TRUE(commit_or.ok()) << commit_or.status();

  int64_t after = absl::ToUnixSeconds(absl::Now());

  // Read the record from the index to find the artifact_id, then read the artifact.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto idx_def = FindIndexDef(*tcr_desc, "transaction_commit_by_id");
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", txn_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  std::string canonical = storage_.GetCanonicalBranch();
  auto idx_obj = ReadIndexObject(storage_, canonical, GenesisIds::kTransactionCommitById, *encoded_key, idx_def, *tcr_desc);
  ASSERT_EQ(idx_obj.rows.size(), 1);
  uint64_t artifact_id = idx_obj.rows[0].artifact_id;

  auto stored = artifact::ReadStoredArtifact(&storage_, canonical, artifact_id);
  ASSERT_TRUE(stored.ok()) << stored.status();
  EXPECT_EQ(stored->type_name(), "artifact_system.TransactionCommitRecord");

  TransactionCommitRecord record;
  ASSERT_TRUE(record.ParseFromString(stored->payload()));
  EXPECT_EQ(record.transaction_id(), txn_id);
  EXPECT_GE(record.committed_at(), before);
  EXPECT_LE(record.committed_at(), after);
}

TEST_F(TransactionCommitRecordTest, DoubleCommitReturnsNotFound) {
  auto txn_or = manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& txn_id = *txn_or;

  ASSERT_TRUE(storage_.PutObject(txn_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(txn_id, "test write").ok());

  // First commit succeeds.
  auto commit1 = manager_->CommitTransaction(txn_id);
  ASSERT_TRUE(commit1.ok()) << commit1.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit1));

  // Second commit of same transaction_id returns NotFound — the transaction
  // was removed from the local map after the first commit.
  auto commit2 = manager_->CommitTransaction(txn_id);
  EXPECT_FALSE(commit2.ok());
  EXPECT_TRUE(absl::IsNotFound(commit2.status())) << commit2.status();

  // Verify exactly one record exists on the canonical branch.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto idx_def = FindIndexDef(*tcr_desc, "transaction_commit_by_id");
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", txn_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  std::string canonical = storage_.GetCanonicalBranch();
  auto idx_obj = ReadIndexObject(storage_, canonical, GenesisIds::kTransactionCommitById, *encoded_key, idx_def, *tcr_desc);
  EXPECT_EQ(idx_obj.rows.size(), 1) << "should have exactly one record, not duplicates";
}

TEST_F(TransactionCommitRecordTest, DoubleCommitSubTransactionReturnsNotFound) {
  auto parent_or = manager_->CreateTransaction();
  ASSERT_TRUE(parent_or.ok()) << parent_or.status();
  const std::string& parent_id = *parent_or;

  auto child_or = manager_->CreateTransaction(/*parent_snapshot_id=*/std::nullopt, /*parent_transaction_id=*/parent_id);
  ASSERT_TRUE(child_or.ok()) << child_or.status();
  const std::string& child_id = *child_or;

  ASSERT_TRUE(storage_.PutObject(child_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(child_id, "test write").ok());

  // First commit of sub-transaction succeeds.
  auto commit1 = manager_->CommitTransaction(child_id);
  ASSERT_TRUE(commit1.ok()) << commit1.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit1));

  // Second commit returns NotFound.
  auto commit2 = manager_->CommitTransaction(child_id);
  EXPECT_FALSE(commit2.ok());
  EXPECT_TRUE(absl::IsNotFound(commit2.status())) << commit2.status();

  // Verify only one record on the parent branch.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto idx_def = FindIndexDef(*tcr_desc, "transaction_commit_by_id");
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", child_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  auto idx_obj = ReadIndexObject(storage_, parent_id, GenesisIds::kTransactionCommitById, *encoded_key, idx_def, *tcr_desc);
  EXPECT_EQ(idx_obj.rows.size(), 1);

  // Clean up: commit parent.
  auto parent_commit = manager_->CommitTransaction(parent_id);
  ASSERT_TRUE(parent_commit.ok()) << parent_commit.status();
}

TEST_F(TransactionCommitRecordTest, ImplicitTransactionDoesNotCreateRecord) {
  auto result_or =
      manager_->RunImplicitTransaction([&](const std::string& branch) -> absl::Status { return storage_.PutObject(branch, "test/implicit.txt", "value"); });
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*result_or));

  const auto& success = std::get<TransactionManager::CommitSuccess>(*result_or);

  // The implicit transaction_id should NOT have a commit record.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", success.transaction_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  std::string canonical = storage_.GetCanonicalBranch();
  EXPECT_FALSE(IndexEntryExists(storage_, canonical, GenesisIds::kTransactionCommitById, *encoded_key));
}

TEST_F(TransactionCommitRecordTest, RecordIsQueryableViaIndex) {
  auto txn_or = manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& txn_id = *txn_or;

  ASSERT_TRUE(storage_.PutObject(txn_id, "test/data.txt", "value").ok());
  ASSERT_TRUE(storage_.Commit(txn_id, "test write").ok());

  auto commit_or = manager_->CommitTransaction(txn_id);
  ASSERT_TRUE(commit_or.ok()) << commit_or.status();

  // Query via the unique index.
  const auto* tcr_desc = TransactionCommitRecord::descriptor();
  auto idx_def = FindIndexDef(*tcr_desc, "transaction_commit_by_id");
  auto encoded_key = encoding::EncodeSingleStringKey(*tcr_desc, "transaction_id", txn_id);
  ASSERT_TRUE(encoded_key.ok()) << encoded_key.status();

  std::string canonical = storage_.GetCanonicalBranch();
  auto idx_obj = ReadIndexObject(storage_, canonical, GenesisIds::kTransactionCommitById, *encoded_key, idx_def, *tcr_desc);
  ASSERT_EQ(idx_obj.rows.size(), 1);

  // The artifact should be readable.
  uint64_t artifact_id = idx_obj.rows[0].artifact_id;
  auto stored = artifact::ReadStoredArtifact(&storage_, canonical, artifact_id);
  ASSERT_TRUE(stored.ok()) << stored.status();

  TransactionCommitRecord record;
  ASSERT_TRUE(record.ParseFromString(stored->payload()));
  EXPECT_EQ(record.transaction_id(), txn_id);
}

TEST_F(TransactionCommitRecordTest, GenesisIncludesTransactionCommitRecordType) {
  // TypeDefinition artifact exists.
  std::string canonical = storage_.GetCanonicalBranch();
  auto td_data = storage_.GetObject(canonical, encoding::ArtifactPath(GenesisIds::kTransactionCommitRecordTypeDef));
  ASSERT_TRUE(td_data.ok()) << td_data.status();

  StoredArtifact td_envelope;
  ASSERT_TRUE(td_envelope.ParseFromString(*td_data));
  EXPECT_EQ(td_envelope.type_name(), "artifact_system.TypeDefinition");

  TypeDefinition td;
  ASSERT_TRUE(td.ParseFromString(td_envelope.payload()));
  EXPECT_EQ(td.type_name(), "artifact_system.TransactionCommitRecord");
  EXPECT_EQ(td.current_version_id(), GenesisIds::kTransactionCommitRecordTypeVersionDef);
  EXPECT_TRUE(td.deny_create());
  EXPECT_TRUE(td.deny_update());
  EXPECT_TRUE(td.deny_delete());

  // TypeVersionDefinition artifact exists.
  auto tvd_data = storage_.GetObject(canonical, encoding::ArtifactPath(GenesisIds::kTransactionCommitRecordTypeVersionDef));
  ASSERT_TRUE(tvd_data.ok()) << tvd_data.status();

  StoredArtifact tvd_envelope;
  ASSERT_TRUE(tvd_envelope.ParseFromString(*tvd_data));
  EXPECT_EQ(tvd_envelope.type_name(), "artifact_system.TypeVersionDefinition");

  TypeVersionDefinition tvd;
  ASSERT_TRUE(tvd.ParseFromString(tvd_envelope.payload()));
  EXPECT_EQ(tvd.type_id(), GenesisIds::kTransactionCommitRecordTypeDef);
  EXPECT_GT(tvd.descriptor_set().file_size(), 0);

  // IndexDefinition artifact exists.
  auto idx_data = storage_.GetObject(canonical, encoding::ArtifactPath(GenesisIds::kTransactionCommitById));
  ASSERT_TRUE(idx_data.ok()) << idx_data.status();

  StoredArtifact idx_envelope;
  ASSERT_TRUE(idx_envelope.ParseFromString(*idx_data));
  EXPECT_EQ(idx_envelope.type_name(), "artifact_system.IndexDefinition");

  IndexDefinition idx;
  ASSERT_TRUE(idx.ParseFromString(idx_envelope.payload()));
  EXPECT_EQ(idx.key_type(), "transaction_commit_by_id");
  EXPECT_TRUE(idx.unique());
}

TEST_F(TransactionCommitRecordTest, GenesisIndexEntriesIncludeNewArtifacts) {
  std::string canonical = storage_.GetCanonicalBranch();
  const auto* td_desc = TypeDefinition::descriptor();
  const auto* idx_desc = IndexDefinition::descriptor();

  // type_name_unique should include TransactionCommitRecord.
  {
    auto idx_def = FindIndexDef(*td_desc, "type_name_unique");
    auto key = encoding::EncodeSingleStringKey(*td_desc, "type_name", "artifact_system.TransactionCommitRecord");
    ASSERT_TRUE(key.ok()) << key.status();
    auto obj = ReadIndexObject(storage_, canonical, GenesisIds::kTypeNameUnique, *key, idx_def, *td_desc);
    ASSERT_EQ(obj.rows.size(), 1);
    EXPECT_EQ(obj.rows[0].artifact_id, GenesisIds::kTransactionCommitRecordTypeDef);
  }

  // all_types should include TransactionCommitRecord.
  {
    auto idx_def = FindIndexDef(*td_desc, "all_types");
    std::vector<uint8_t> empty_key;
    auto obj = ReadIndexObject(storage_, canonical, GenesisIds::kAllTypes, empty_key, idx_def, *td_desc);
    bool found = false;
    for (const auto& row : obj.rows) {
      if (row.artifact_id == GenesisIds::kTransactionCommitRecordTypeDef) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "TransactionCommitRecord TypeDefinition not found in all_types index";
  }

  // index_key_type_unique should include transaction_commit_by_id.
  {
    auto idx_def = FindIndexDef(*idx_desc, "index_key_type_unique");
    auto key = encoding::EncodeSingleStringKey(*idx_desc, "key_type", "transaction_commit_by_id");
    ASSERT_TRUE(key.ok()) << key.status();
    auto obj = ReadIndexObject(storage_, canonical, GenesisIds::kIndexKeyTypeUnique, *key, idx_def, *idx_desc);
    ASSERT_EQ(obj.rows.size(), 1);
    EXPECT_EQ(obj.rows[0].artifact_id, GenesisIds::kTransactionCommitById);
  }

  // all_index_definitions should include the new index.
  {
    auto idx_def = FindIndexDef(*idx_desc, "all_index_definitions");
    std::vector<uint8_t> empty_key;
    auto obj = ReadIndexObject(storage_, canonical, GenesisIds::kAllIndexDefinitions, empty_key, idx_def, *idx_desc);
    bool found = false;
    for (const auto& row : obj.rows) {
      if (row.artifact_id == GenesisIds::kTransactionCommitById) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "transaction_commit_by_id not found in all_index_definitions index";
  }
}

} // namespace
} // namespace artifact_system::testing

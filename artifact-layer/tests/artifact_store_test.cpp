#include "artifact/artifact_store.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "id/id_allocator_interface.h"
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::artifact::ArtifactStore;
using artifact_system::artifact::BatchGetEntry;
using artifact_system::artifact::CreateResult;
using artifact_system::artifact::GetResult;
using artifact_system::artifact::WriteResult;
using artifact_system::transaction::TransactionManager;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a FileDescriptorSet for a simple test message with no indexes.
// The message is named "test.SimpleTestArtifact" with a single string field "name".
google::protobuf::FileDescriptorSet BuildSimpleTestDescriptorSet() {
  google::protobuf::FileDescriptorProto file;
  file.set_name("simple_test_artifact.proto");
  file.set_syntax("proto3");
  file.set_package("test");

  auto* message = file.add_message_type();
  message->set_name("SimpleTestArtifact");

  auto* name_field = message->add_field();
  name_field->set_name("name");
  name_field->set_number(1);
  name_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* value_field = message->add_field();
  value_field->set_name("value");
  value_field->set_number(2);
  value_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  value_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  google::protobuf::FileDescriptorSet fds;
  *fds.add_file() = file;
  return fds;
}

// Build a serialized test payload using a DynamicMessage for SimpleTestArtifact.
std::string MakeSimpleTestPayload(const std::string& name, const std::string& value = "") {
  google::protobuf::FileDescriptorSet fds = BuildSimpleTestDescriptorSet();
  google::protobuf::DescriptorPool pool;
  const auto* file_desc = pool.BuildFile(fds.file(0));
  if (file_desc == nullptr)
    return "";
  const auto* desc = file_desc->FindMessageTypeByName("SimpleTestArtifact");
  if (desc == nullptr)
    return "";

  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  const auto* reflection = msg->GetReflection();
  reflection->SetString(msg.get(), desc->FindFieldByName("name"), name);
  if (!value.empty()) {
    reflection->SetString(msg.get(), desc->FindFieldByName("value"), value);
  }
  return msg->SerializeAsString();
}

// Extract the ArtifactNotFoundError payload from a NOT_FOUND Status.
std::optional<ArtifactNotFoundError> ExtractNotFoundError(const absl::Status& status) {
  auto payload = status.GetPayload("type.googleapis.com/artifact_system.ArtifactNotFoundError");
  if (!payload.has_value())
    return std::nullopt;
  ArtifactNotFoundError error;
  if (!error.ParseFromString(std::string(payload->Flatten())))
    return std::nullopt;
  return error;
}

// ---------------------------------------------------------------------------
// Fixture: bootstraps a minimal type system on the canonical branch.
//
// Registers two meta-types (TypeDefinition, TypeVersionDefinition) and one
// simple test type ("test.SimpleTestArtifact") that has no indexes, making
// CRUD operations straightforward to test.
// ---------------------------------------------------------------------------

class ArtifactStoreTest : public ::testing::Test {
protected:
  // Well-known bootstrap IDs (low range, below the MockIdAllocator start).
  static constexpr uint64_t kTypeDefOfTypeDefId = 1;
  static constexpr uint64_t kTVDForTypeDefId = 2;
  static constexpr uint64_t kTypeDefOfTVDId = 3;
  static constexpr uint64_t kTVDForTVDId = 4;
  // Test type artifacts.
  static constexpr uint64_t kTestTypeDefId = 5;
  static constexpr uint64_t kTestTVDId = 6;

  void SetUp() override {
    storage_ = std::make_unique<MemoryStorage>();
    transaction_manager_ = std::make_unique<TransactionManager>(storage_.get());
    id_allocator_ = std::make_unique<MockIdAllocator>(1000);

    BootstrapTypeSystem();

    ArtifactStore::Options options;
    options.bypass_mutation_check = true;
    store_ = std::make_unique<ArtifactStore>(storage_.get(), transaction_manager_.get(), id_allocator_.get(), options);
  }

  uint64_t TestVersionId() const {
    return kTestTVDId;
  }

  std::unique_ptr<MemoryStorage> storage_;
  std::unique_ptr<TransactionManager> transaction_manager_;
  std::unique_ptr<MockIdAllocator> id_allocator_;
  std::unique_ptr<ArtifactStore> store_;

private:
  void WriteStoredArtifact(const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name, const std::string& payload) {
    ASSERT_TRUE(storage_->PutObject(branch, encoding::ArtifactPath(artifact_id), artifact::SerializeStoredArtifact(version_id, type_name, payload)).ok());
  }

  void BootstrapTypeSystem() {
    const std::string branch = storage_->GetCanonicalBranch();

    // -- TypeDefinition for TypeDefinition ------------------------------------
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.TypeDefinition");
      td.set_deny_create(false);
      td.set_deny_update(false);
      td.set_deny_delete(false);
      WriteStoredArtifact(branch, kTypeDefOfTypeDefId, kTVDForTypeDefId, "TypeDefinition", td.SerializeAsString());
    }

    // -- TypeVersionDefinition for TypeDefinition -----------------------------
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kTypeDefOfTypeDefId);
      *tvd.mutable_descriptor_set() = artifact::BuildDescriptorSet(TypeDefinition::descriptor());
      WriteStoredArtifact(branch, kTVDForTypeDefId, kTVDForTVDId, "TypeVersionDefinition", tvd.SerializeAsString());
    }

    // -- TypeDefinition for TypeVersionDefinition -----------------------------
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.TypeVersionDefinition");
      td.set_deny_create(false);
      td.set_deny_update(false);
      td.set_deny_delete(false);
      WriteStoredArtifact(branch, kTypeDefOfTVDId, kTVDForTVDId, "TypeDefinition", td.SerializeAsString());
    }

    // -- TypeVersionDefinition for TypeVersionDefinition ----------------------
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kTypeDefOfTVDId);
      *tvd.mutable_descriptor_set() = artifact::BuildDescriptorSet(TypeVersionDefinition::descriptor());
      WriteStoredArtifact(branch, kTVDForTVDId, kTVDForTVDId, "TypeVersionDefinition", tvd.SerializeAsString());
    }

    // -- TypeDefinition for test.SimpleTestArtifact ---------------------------
    {
      TypeDefinition td;
      td.set_type_name("test.SimpleTestArtifact");
      td.set_deny_create(false);
      td.set_deny_update(false);
      td.set_deny_delete(false);
      WriteStoredArtifact(branch, kTestTypeDefId, kTVDForTypeDefId, "TypeDefinition", td.SerializeAsString());
    }

    // -- TypeVersionDefinition for test.SimpleTestArtifact --------------------
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kTestTypeDefId);
      *tvd.mutable_descriptor_set() = BuildSimpleTestDescriptorSet();
      WriteStoredArtifact(branch, kTestTVDId, kTVDForTVDId, "TypeVersionDefinition", tvd.SerializeAsString());
    }

    ASSERT_TRUE(storage_->Commit(branch, "bootstrap type system").ok());
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(ArtifactStoreTest, CreateAndGetArtifact) {
  const std::string payload = MakeSimpleTestPayload("TestType");

  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();

  const CreateResult& created = *create_or;
  EXPECT_GE(created.artifact_id, 1000U);
  EXPECT_FALSE(created.snapshot_id.empty());

  ReadContext ctx;
  auto get_or = store_->GetArtifact(created.artifact_id, ctx);
  ASSERT_TRUE(get_or.ok()) << get_or.status();

  const GetResult& got = *get_or;
  EXPECT_EQ(got.artifact_id, created.artifact_id);
  EXPECT_EQ(got.type_name, "test.SimpleTestArtifact");
  EXPECT_EQ(got.version_id, TestVersionId());
  EXPECT_EQ(got.payload, payload);
}

TEST_F(ArtifactStoreTest, CreateArtifactImplicitTransaction) {
  const std::string payload = MakeSimpleTestPayload("ImplicitType");

  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();

  EXPECT_FALSE(create_or->snapshot_id.empty());

  ReadContext ctx;
  auto get_or = store_->GetArtifact(create_or->artifact_id, ctx);
  ASSERT_TRUE(get_or.ok()) << get_or.status();
  EXPECT_EQ(get_or->payload, payload);
}

TEST_F(ArtifactStoreTest, CreateArtifactExplicitTransaction) {
  auto tx_id_or = transaction_manager_->CreateTransaction();
  ASSERT_TRUE(tx_id_or.ok());
  const std::string& tx_id = *tx_id_or;

  const std::string payload = MakeSimpleTestPayload("ExplicitType");
  auto create_or = store_->CreateArtifact(TestVersionId(), payload, tx_id);
  ASSERT_TRUE(create_or.ok()) << create_or.status();

  const uint64_t artifact_id = create_or->artifact_id;

  // Visible via transaction read context.
  ReadContext tx_ctx;
  tx_ctx.set_transaction_id(tx_id);
  auto get_in_tx_or = store_->GetArtifact(artifact_id, tx_ctx);
  ASSERT_TRUE(get_in_tx_or.ok()) << get_in_tx_or.status();
  EXPECT_EQ(get_in_tx_or->payload, payload);

  // Not visible on canonical yet.
  ReadContext canonical_ctx;
  auto get_canonical_or = store_->GetArtifact(artifact_id, canonical_ctx);
  ASSERT_FALSE(get_canonical_or.ok());
  EXPECT_EQ(get_canonical_or.status().code(), absl::StatusCode::kNotFound);

  // Commit.
  auto commit_or = transaction_manager_->CommitTransaction(tx_id);
  ASSERT_TRUE(commit_or.ok());
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_or));

  // Now visible on canonical.
  auto get_after_commit_or = store_->GetArtifact(artifact_id, canonical_ctx);
  ASSERT_TRUE(get_after_commit_or.ok()) << get_after_commit_or.status();
  EXPECT_EQ(get_after_commit_or->payload, payload);
}

TEST_F(ArtifactStoreTest, GetArtifactNotFound) {
  ReadContext ctx;
  auto get_or = store_->GetArtifact(999999, ctx);
  ASSERT_FALSE(get_or.ok());
  EXPECT_EQ(get_or.status().code(), absl::StatusCode::kNotFound);

  auto nfe = ExtractNotFoundError(get_or.status());
  ASSERT_TRUE(nfe.has_value());
  EXPECT_EQ(nfe->artifact_id(), 999999U);
  EXPECT_FALSE(nfe->tombstoned());
}

TEST_F(ArtifactStoreTest, GetTombstonedArtifact) {
  const std::string payload = MakeSimpleTestPayload("WillBeDeleted");
  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();
  const uint64_t artifact_id = create_or->artifact_id;

  auto delete_or = store_->DeleteArtifact(artifact_id);
  ASSERT_TRUE(delete_or.ok()) << delete_or.status();

  ReadContext ctx;
  auto get_or = store_->GetArtifact(artifact_id, ctx);
  ASSERT_FALSE(get_or.ok());
  EXPECT_EQ(get_or.status().code(), absl::StatusCode::kNotFound);

  auto nfe = ExtractNotFoundError(get_or.status());
  ASSERT_TRUE(nfe.has_value());
  EXPECT_EQ(nfe->artifact_id(), artifact_id);
  EXPECT_TRUE(nfe->tombstoned());
}

TEST_F(ArtifactStoreTest, BatchGetArtifacts) {
  const std::string payload_a = MakeSimpleTestPayload("BatchA");
  const std::string payload_b = MakeSimpleTestPayload("BatchB");

  auto create_a_or = store_->CreateArtifact(TestVersionId(), payload_a);
  ASSERT_TRUE(create_a_or.ok()) << create_a_or.status();
  auto create_b_or = store_->CreateArtifact(TestVersionId(), payload_b);
  ASSERT_TRUE(create_b_or.ok()) << create_b_or.status();

  ReadContext ctx;
  std::vector<uint64_t> ids = {create_a_or->artifact_id, create_b_or->artifact_id};
  auto batch_or = store_->BatchGetArtifacts(ids, ctx);
  ASSERT_TRUE(batch_or.ok()) << batch_or.status();

  const auto& results = *batch_or;
  ASSERT_EQ(results.size(), 2U);

  ASSERT_TRUE(std::holds_alternative<GetResult>(results[0].result));
  const auto& got_a = std::get<GetResult>(results[0].result);
  EXPECT_EQ(got_a.artifact_id, create_a_or->artifact_id);
  EXPECT_EQ(got_a.payload, payload_a);

  ASSERT_TRUE(std::holds_alternative<GetResult>(results[1].result));
  const auto& got_b = std::get<GetResult>(results[1].result);
  EXPECT_EQ(got_b.artifact_id, create_b_or->artifact_id);
  EXPECT_EQ(got_b.payload, payload_b);
}

TEST_F(ArtifactStoreTest, BatchGetMixed) {
  const std::string payload = MakeSimpleTestPayload("MixedExisting");
  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();
  const uint64_t existing_id = create_or->artifact_id;

  const std::string tombstone_payload = MakeSimpleTestPayload("MixedTombstoned");
  auto create_tombstone_or = store_->CreateArtifact(TestVersionId(), tombstone_payload);
  ASSERT_TRUE(create_tombstone_or.ok()) << create_tombstone_or.status();
  const uint64_t tombstoned_id = create_tombstone_or->artifact_id;
  auto delete_or = store_->DeleteArtifact(tombstoned_id);
  ASSERT_TRUE(delete_or.ok()) << delete_or.status();

  const uint64_t missing_id = 888888;

  ReadContext ctx;
  std::vector<uint64_t> ids = {existing_id, missing_id, tombstoned_id};
  auto batch_or = store_->BatchGetArtifacts(ids, ctx);
  ASSERT_TRUE(batch_or.ok()) << batch_or.status();

  const auto& results = *batch_or;
  ASSERT_EQ(results.size(), 3U);

  ASSERT_TRUE(std::holds_alternative<GetResult>(results[0].result));
  EXPECT_EQ(std::get<GetResult>(results[0].result).artifact_id, existing_id);

  ASSERT_TRUE(std::holds_alternative<ArtifactNotFoundError>(results[1].result));
  const auto& nfe_missing = std::get<ArtifactNotFoundError>(results[1].result);
  EXPECT_EQ(nfe_missing.artifact_id(), missing_id);
  EXPECT_FALSE(nfe_missing.tombstoned());

  ASSERT_TRUE(std::holds_alternative<ArtifactNotFoundError>(results[2].result));
  const auto& nfe_tombstoned = std::get<ArtifactNotFoundError>(results[2].result);
  EXPECT_EQ(nfe_tombstoned.artifact_id(), tombstoned_id);
  EXPECT_TRUE(nfe_tombstoned.tombstoned());
}

TEST_F(ArtifactStoreTest, UpdateArtifact) {
  const std::string original_payload = MakeSimpleTestPayload("OriginalName");
  auto create_or = store_->CreateArtifact(TestVersionId(), original_payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();
  const uint64_t artifact_id = create_or->artifact_id;

  const std::string updated_payload = MakeSimpleTestPayload("UpdatedName", "new-value");
  auto update_or = store_->UpdateArtifact(artifact_id, TestVersionId(), updated_payload);
  ASSERT_TRUE(update_or.ok()) << update_or.status();
  EXPECT_FALSE(update_or->snapshot_id.empty());

  ReadContext ctx;
  auto get_or = store_->GetArtifact(artifact_id, ctx);
  ASSERT_TRUE(get_or.ok()) << get_or.status();
  EXPECT_EQ(get_or->payload, updated_payload);
}

TEST_F(ArtifactStoreTest, UpdateArtifactNotFound) {
  const std::string payload = MakeSimpleTestPayload("Ghost");
  auto update_or = store_->UpdateArtifact(999999, TestVersionId(), payload);
  ASSERT_FALSE(update_or.ok());
  EXPECT_EQ(update_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(ArtifactStoreTest, DeleteArtifact) {
  const std::string payload = MakeSimpleTestPayload("ToDelete");
  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();
  const uint64_t artifact_id = create_or->artifact_id;

  auto delete_or = store_->DeleteArtifact(artifact_id);
  ASSERT_TRUE(delete_or.ok()) << delete_or.status();
  EXPECT_FALSE(delete_or->snapshot_id.empty());

  ReadContext ctx;
  auto get_or = store_->GetArtifact(artifact_id, ctx);
  ASSERT_FALSE(get_or.ok());
  EXPECT_EQ(get_or.status().code(), absl::StatusCode::kNotFound);

  auto nfe = ExtractNotFoundError(get_or.status());
  ASSERT_TRUE(nfe.has_value());
  EXPECT_TRUE(nfe->tombstoned());
}

TEST_F(ArtifactStoreTest, DeleteAlreadyTombstoned) {
  const std::string payload = MakeSimpleTestPayload("DeleteTwice");
  auto create_or = store_->CreateArtifact(TestVersionId(), payload);
  ASSERT_TRUE(create_or.ok()) << create_or.status();
  const uint64_t artifact_id = create_or->artifact_id;

  auto first_delete_or = store_->DeleteArtifact(artifact_id);
  ASSERT_TRUE(first_delete_or.ok()) << first_delete_or.status();

  auto second_delete_or = store_->DeleteArtifact(artifact_id);
  ASSERT_FALSE(second_delete_or.ok());
  EXPECT_EQ(second_delete_or.status().code(), absl::StatusCode::kNotFound);

  auto nfe = ExtractNotFoundError(second_delete_or.status());
  ASSERT_TRUE(nfe.has_value());
  EXPECT_TRUE(nfe->tombstoned());
}

} // namespace
} // namespace artifact_system::testing

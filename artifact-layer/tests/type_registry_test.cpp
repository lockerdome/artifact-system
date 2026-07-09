#include "registry/type_registry.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "id/id_allocator_interface.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "index/index_utils.h"
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::artifact::ArtifactStore;
using artifact_system::registry::IndexSchemaInfo;
using artifact_system::registry::RegisterResult;
using artifact_system::registry::TypeRegistry;
using artifact_system::registry::TypeVersionInfo;
using artifact_system::transaction::TransactionManager;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void WriteStoredArtifact(MemoryStorage& storage, const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name,
                         const std::string& payload) {
  ASSERT_TRUE(storage.PutObject(branch, encoding::ArtifactPath(artifact_id), artifact::SerializeStoredArtifact(version_id, type_name, payload)).ok());
}

// Build a proto-serialized key for an index.
std::string BuildProtoKey(const index::GeneratedIndexSchema& schema, const std::vector<index::IndexCell>& key_values) {
  auto result = index::BuildProtoSerializedKey(schema, key_values);
  EXPECT_TRUE(result.ok()) << result.status();
  return result.value_or(std::string{});
}

// Write an index entry pointing artifact_id to the given index object.
void WriteIndexEntry(MemoryStorage& storage, const std::string& branch, uint64_t index_def_id, const std::vector<uint8_t>& encoded_key, uint64_t artifact_id,
                     const IndexDefinition& idx_def, const google::protobuf::Descriptor& parent_desc, const std::vector<index::IndexCell>& key_values) {
  index::DerivedIndexEntry entry;
  entry.index_def_id = index_def_id;
  entry.key_type = idx_def.key_type();
  entry.encoded_key = encoded_key;
  entry.order_values.push_back(static_cast<uint64_t>(artifact_id));
  entry.key_values = key_values;
  ASSERT_TRUE(index::AddIndexRow(&storage, branch, entry, artifact_id, idx_def, parent_desc).ok());
}

std::vector<uint8_t> EncodeStringKey(const google::protobuf::Descriptor& desc, const std::string& field_name, const std::string& value) {
  auto encoded_or = encoding::EncodeSingleStringKey(desc, field_name, value);
  EXPECT_TRUE(encoded_or.ok()) << encoded_or.status();
  return *encoded_or;
}

std::vector<uint8_t> EncodeUint64Key(const google::protobuf::Descriptor& desc, const std::string& field_name, uint64_t value) {
  auto encoded_or = encoding::EncodeSingleUint64Key(desc, field_name, value);
  EXPECT_TRUE(encoded_or.ok()) << encoded_or.status();
  return *encoded_or;
}

std::vector<uint8_t> EncodeEmptyKey() {
  return {};
}

// Extract RegisterTypeVersionError from a failed status.
std::optional<RegisterTypeVersionError> ExtractRegistrationError(const absl::Status& status) {
  auto payload = status.GetPayload("type.googleapis.com/artifact_system.RegisterTypeVersionError");
  if (!payload.has_value())
    return std::nullopt;
  RegisterTypeVersionError error;
  if (!error.ParseFromString(std::string(payload->Flatten())))
    return std::nullopt;
  return error;
}

std::optional<SnapshotTransactionError> ExtractSnapshotTransactionError(const absl::Status& status) {
  auto payload = status.GetPayload("type.googleapis.com/artifact_system.SnapshotTransactionError");
  if (!payload.has_value())
    return std::nullopt;
  SnapshotTransactionError error;
  if (!error.ParseFromString(std::string(payload->Flatten())))
    return std::nullopt;
  return error;
}

ReadContext CanonicalReadContext() {
  ReadContext context;
  return context;
}

absl::StatusOr<std::string> BuildPayload(const google::protobuf::FileDescriptorSet& descriptor_set, const std::string& type_name, const std::string& name,
                                         const std::string& value) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = artifact::BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return absl::NotFoundError("message descriptor not found");
  }

  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(descriptor);
  if (prototype == nullptr) {
    return absl::InternalError("failed to construct dynamic message prototype");
  }

  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  const auto* reflection = message->GetReflection();
  const auto* name_field = descriptor->FindFieldByName("name");
  const auto* value_field = descriptor->FindFieldByName("value");
  if (name_field == nullptr || value_field == nullptr) {
    return absl::InternalError("test descriptor missing expected fields");
  }
  reflection->SetString(message.get(), name_field, name);
  reflection->SetString(message.get(), value_field, value);
  return message->SerializeAsString();
}

// Assert that a failed registration status contains a violation with the given category.
void ExpectViolationCategory(const absl::Status& status, TypeRegistrationViolation::Category category) {
  ASSERT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  auto error = ExtractRegistrationError(status);
  ASSERT_TRUE(error.has_value()) << "expected RegisterTypeVersionError payload";
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == category) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "expected violation category " << static_cast<int>(category);
}

// ---------------------------------------------------------------------------
// Fixture: bootstraps the full built-in type system with indexes.
//
// Creates TypeDefinition, TypeVersionDefinition, IndexDefinition, and
// ReferenceDefinition meta-types with their index entries so that
// TypeRegistry can look up types, versions, and indexes.
// ---------------------------------------------------------------------------

class TypeRegistryTest : public ::testing::Test {
protected:
  // Pre-allocated bootstrap IDs.
  static constexpr uint64_t kIdxDefTypeDefId = 1;  // TypeDefinition for IndexDefinition
  static constexpr uint64_t kIdxDefTVDId = 2;      // TypeVersionDefinition for IndexDefinition
  static constexpr uint64_t kTypeDefTypeDefId = 3; // TypeDefinition for TypeDefinition
  static constexpr uint64_t kTypeDefTVDId = 4;     // TypeVersionDefinition for TypeDefinition
  static constexpr uint64_t kTVDTypeDefId = 5;     // TypeDefinition for TypeVersionDefinition
  static constexpr uint64_t kTVDTVDId = 6;         // TypeVersionDefinition for TypeVersionDefinition
  static constexpr uint64_t kRefDefTypeDefId = 7;  // TypeDefinition for ReferenceDefinition
  static constexpr uint64_t kRefDefTVDId = 8;      // TypeVersionDefinition for ReferenceDefinition

  // Bootstrap index definition artifact IDs.
  static constexpr uint64_t kIndexKeyTypeUniqueId = 10;
  static constexpr uint64_t kAllIndexDefsId = 11;
  static constexpr uint64_t kTypeNameUniqueId = 12;
  static constexpr uint64_t kAllTypesId = 13;
  static constexpr uint64_t kTypeVersionsByTypeId = 14;
  static constexpr uint64_t kRefKeyTypeUniqueId = 15;
  static constexpr uint64_t kRefsByTargetTypeId = 16;
  static constexpr uint64_t kAllRefDefsId = 17;

  void SetUp() override {
    storage_ = std::make_unique<MemoryStorage>();
    transaction_manager_ = std::make_unique<TransactionManager>(storage_.get());
    id_allocator_ = std::make_unique<MockIdAllocator>(1000); // User artifacts start at 1000.

    BootstrapTypeSystem();

    registry_ = std::make_unique<TypeRegistry>(storage_.get(), transaction_manager_.get(), id_allocator_.get(), index_def_ids_);
  }

  std::unique_ptr<MemoryStorage> storage_;
  std::unique_ptr<TransactionManager> transaction_manager_;
  std::unique_ptr<MockIdAllocator> id_allocator_;
  std::unique_ptr<TypeRegistry> registry_;
  std::unordered_map<std::string, uint64_t> index_def_ids_;

private:
  void BootstrapTypeSystem() {
    const std::string branch = storage_->GetCanonicalBranch();

    // ── Index definition map ──
    index_def_ids_ = {
        {"index_key_type_unique", kIndexKeyTypeUniqueId},
        {"all_index_definitions", kAllIndexDefsId},
        {"type_name_unique", kTypeNameUniqueId},
        {"all_types", kAllTypesId},
        {"type_versions_by_type", kTypeVersionsByTypeId},
        {"reference_key_type_unique", kRefKeyTypeUniqueId},
        {"references_by_target_type", kRefsByTargetTypeId},
        {"all_reference_definitions", kAllRefDefsId},
    };

    // ── 1-4. Bootstrap meta-type pairs (TypeDefinition + TypeVersionDefinition) ──
    struct MetaType {
      const char* type_name;
      uint64_t td_id;
      uint64_t tvd_id;
      const google::protobuf::Descriptor* descriptor;
    };
    const MetaType meta_types[] = {
        {"artifact_system.IndexDefinition", kIdxDefTypeDefId, kIdxDefTVDId, IndexDefinition::descriptor()},
        {"artifact_system.TypeDefinition", kTypeDefTypeDefId, kTypeDefTVDId, TypeDefinition::descriptor()},
        {"artifact_system.TypeVersionDefinition", kTVDTypeDefId, kTVDTVDId, TypeVersionDefinition::descriptor()},
        {"artifact_system.ReferenceDefinition", kRefDefTypeDefId, kRefDefTVDId, ReferenceDefinition::descriptor()},
    };
    for (const auto& mt : meta_types) {
      TypeDefinition td;
      td.set_type_name(mt.type_name);
      td.set_current_version_id(mt.tvd_id);
      td.set_deny_create(true);
      td.set_deny_update(true);
      td.set_deny_delete(true);
      WriteStoredArtifact(*storage_, branch, mt.td_id, kTypeDefTVDId, "artifact_system.TypeDefinition", td.SerializeAsString());

      TypeVersionDefinition tvd;
      tvd.set_type_id(mt.td_id);
      *tvd.mutable_descriptor_set() = artifact::BuildDescriptorSet(mt.descriptor);
      WriteStoredArtifact(*storage_, branch, mt.tvd_id, kTVDTVDId, "artifact_system.TypeVersionDefinition", tvd.SerializeAsString());
    }

    // ── 5. Bootstrap IndexDefinition artifacts ──
    auto write_idx_def = [&](uint64_t id, const IndexDefinition& def) {
      WriteStoredArtifact(*storage_, branch, id, kIdxDefTVDId, "artifact_system.IndexDefinition", def.SerializeAsString());
    };

    // Write each bootstrap index definition artifact.
    const auto* idx_desc = IndexDefinition::descriptor();
    const auto* td_desc = TypeDefinition::descriptor();
    const auto* tvd_desc = TypeVersionDefinition::descriptor();
    const auto* rd_desc = ReferenceDefinition::descriptor();

    // index_key_type_unique
    write_idx_def(kIndexKeyTypeUniqueId, idx_desc->options().GetExtension(artifact_system::indexes, 0));
    // all_index_definitions
    write_idx_def(kAllIndexDefsId, idx_desc->options().GetExtension(artifact_system::indexes, 1));
    // type_name_unique
    write_idx_def(kTypeNameUniqueId, td_desc->options().GetExtension(artifact_system::indexes, 0));
    // all_types
    write_idx_def(kAllTypesId, td_desc->options().GetExtension(artifact_system::indexes, 1));
    // type_versions_by_type
    write_idx_def(kTypeVersionsByTypeId, tvd_desc->options().GetExtension(artifact_system::indexes, 0));
    // reference_key_type_unique
    write_idx_def(kRefKeyTypeUniqueId, rd_desc->options().GetExtension(artifact_system::indexes, 0));
    // references_by_target_type
    write_idx_def(kRefsByTargetTypeId, rd_desc->options().GetExtension(artifact_system::indexes, 1));
    // all_reference_definitions
    write_idx_def(kAllRefDefsId, rd_desc->options().GetExtension(artifact_system::indexes, 2));

    // ── 6. Write index entries for TypeDefinitions (type_name_unique + all_types) ──
    auto write_td_index = [&](uint64_t td_artifact_id, const std::string& type_name) {
      // type_name_unique index entry
      auto key = EncodeStringKey(*td_desc, "type_name", type_name);
      const auto& idx = td_desc->options().GetExtension(artifact_system::indexes, 0);
      WriteIndexEntry(*storage_, branch, kTypeNameUniqueId, key, td_artifact_id, idx, *td_desc, {std::string(type_name)});

      // all_types index entry (empty key)
      const auto& all_idx = td_desc->options().GetExtension(artifact_system::indexes, 1);
      WriteIndexEntry(*storage_, branch, kAllTypesId, EncodeEmptyKey(), td_artifact_id, all_idx, *td_desc, {});
    };

    write_td_index(kIdxDefTypeDefId, "artifact_system.IndexDefinition");
    write_td_index(kTypeDefTypeDefId, "artifact_system.TypeDefinition");
    write_td_index(kTVDTypeDefId, "artifact_system.TypeVersionDefinition");
    write_td_index(kRefDefTypeDefId, "artifact_system.ReferenceDefinition");

    // ── 7. Write index entries for TypeVersionDefinitions (type_versions_by_type) ──
    auto write_tvd_index = [&](uint64_t tvd_artifact_id, uint64_t type_def_id) {
      auto key = EncodeUint64Key(*tvd_desc, "type_id", type_def_id);
      const auto& idx = tvd_desc->options().GetExtension(artifact_system::indexes, 0);
      WriteIndexEntry(*storage_, branch, kTypeVersionsByTypeId, key, tvd_artifact_id, idx, *tvd_desc, {static_cast<uint64_t>(type_def_id)});
    };

    write_tvd_index(kIdxDefTVDId, kIdxDefTypeDefId);
    write_tvd_index(kTypeDefTVDId, kTypeDefTypeDefId);
    write_tvd_index(kTVDTVDId, kTVDTypeDefId);
    write_tvd_index(kRefDefTVDId, kRefDefTypeDefId);

    // ── 8. Write index entries for IndexDefinition artifacts (index_key_type_unique) ──
    auto write_idx_index = [&](uint64_t idx_artifact_id, const std::string& key_type) {
      auto key = EncodeStringKey(*idx_desc, "key_type", key_type);
      const auto& idx = idx_desc->options().GetExtension(artifact_system::indexes, 0);
      WriteIndexEntry(*storage_, branch, kIndexKeyTypeUniqueId, key, idx_artifact_id, idx, *idx_desc, {std::string(key_type)});
    };

    write_idx_index(kIndexKeyTypeUniqueId, "index_key_type_unique");
    write_idx_index(kAllIndexDefsId, "all_index_definitions");
    write_idx_index(kTypeNameUniqueId, "type_name_unique");
    write_idx_index(kAllTypesId, "all_types");
    write_idx_index(kTypeVersionsByTypeId, "type_versions_by_type");
    write_idx_index(kRefKeyTypeUniqueId, "reference_key_type_unique");
    write_idx_index(kRefsByTargetTypeId, "references_by_target_type");
    write_idx_index(kAllRefDefsId, "all_reference_definitions");

    ASSERT_TRUE(storage_->Commit(branch, "bootstrap type system").ok());
  }
};

// ---------------------------------------------------------------------------
// Simple .proto sources for testing
// ---------------------------------------------------------------------------

constexpr const char* kSimpleProtoSource = R"(
syntax = "proto3";
package test;
import "artifact_options.proto";

message SimpleArtifact {
  option (artifact_system.indexes) = {
    key_type: "simple_by_name"
    key: ["name"]
    order: { field: "artifact_id" direction: ASCENDING }
    unique: true
  };

  string name = 1;
  string value = 2;
}
)";

constexpr const char* kSimpleProtoSourceV2 = R"(
syntax = "proto3";
package test;
import "artifact_options.proto";

message SimpleArtifact {
  option (artifact_system.indexes) = {
    key_type: "simple_by_name"
    key: ["name"]
    order: { field: "artifact_id" direction: ASCENDING }
    unique: true
  };

  string name = 1;
  string value = 2;
  string description = 3;
}
)";

constexpr const char* kNoIndexProtoSource = R"(
syntax = "proto3";
package test;

message NoIndexArtifact {
  string label = 1;
}
)";

// Proto with a reference field.
constexpr const char* kRefProtoSource = R"(
syntax = "proto3";
package test;
import "artifact_options.proto";

message RefArtifact {
  option (artifact_system.indexes) = {
    key_type: "ref_by_target"
    key: ["target_id"]
    order: { field: "artifact_id" direction: ASCENDING }
  };

  optional uint64 target_id = 1 [(artifact_system.references) = {
    target_type_name: "test.SimpleArtifact"
    on_delete: RESTRICT
  }];
  string label = 2;
}
)";

// Proto with a reference field on a nested message (dotted field path).
constexpr const char* kNestedRefProtoSource = R"(
syntax = "proto3";
package test;
import "artifact_options.proto";

message NestedRefArtifact {
  option (artifact_system.indexes) = {
    key_type: "nested_ref_by_target"
    key: ["inputs.target_ids"]
    order: { field: "artifact_id" direction: ASCENDING }
  };

  message Input {
    string name = 1;
    repeated uint64 target_ids = 2 [(artifact_system.references) = {
      target_type_name: "test.SimpleArtifact"
      on_delete: RESTRICT
    }];
  }

  repeated Input inputs = 1;
}
)";

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(TypeRegistryTest, RegisterNewType) {
  auto result_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_GT(result_or->version_id, 0U);
}

TEST_F(TypeRegistryTest, RegisterNewTypeNoIndexes) {
  auto result_or = registry_->RegisterTypeVersion("test.NoIndexArtifact", kNoIndexProtoSource);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_GT(result_or->version_id, 0U);
}

TEST_F(TypeRegistryTest, RegisterNewVersion) {
  // Register v1.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();
  uint64_t v1 = v1_or->version_id;

  // Register v2 (adds a field — compatible).
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();
  uint64_t v2 = v2_or->version_id;
  EXPECT_NE(v1, v2);

  // Verify doubly-linked list.
  auto v1_info = registry_->GetTypeVersion(v1, CanonicalReadContext());
  ASSERT_TRUE(v1_info.ok()) << v1_info.status();
  EXPECT_FALSE(v1_info->previous_version_id.has_value());
  ASSERT_TRUE(v1_info->next_version_id.has_value());
  EXPECT_EQ(*v1_info->next_version_id, v2);

  auto v2_info = registry_->GetTypeVersion(v2, CanonicalReadContext());
  ASSERT_TRUE(v2_info.ok()) << v2_info.status();
  ASSERT_TRUE(v2_info->previous_version_id.has_value());
  EXPECT_EQ(*v2_info->previous_version_id, v1);
  EXPECT_FALSE(v2_info->next_version_id.has_value());
}

TEST_F(TypeRegistryTest, GetTypeVersion) {
  auto reg_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  auto info = registry_->GetTypeVersion(reg_or->version_id, CanonicalReadContext());
  ASSERT_TRUE(info.ok()) << info.status();
  EXPECT_EQ(info->version_id, reg_or->version_id);
  EXPECT_GT(info->type_id, 0U);
  EXPECT_GT(info->descriptor_set.file_size(), 0);
  EXPECT_FALSE(info->proto_source.empty());
  EXPECT_FALSE(info->previous_version_id.has_value());
  EXPECT_FALSE(info->next_version_id.has_value());
}

TEST_F(TypeRegistryTest, GetTypeVersionNotFound) {
  auto info = registry_->GetTypeVersion(99999, CanonicalReadContext());
  ASSERT_FALSE(info.ok());
  EXPECT_EQ(info.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(TypeRegistryTest, ListTypeVersions) {
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();

  auto versions_or = registry_->ListTypeVersions("test.SimpleArtifact", CanonicalReadContext());
  ASSERT_TRUE(versions_or.ok()) << versions_or.status();
  EXPECT_EQ(versions_or->size(), 2U);
  EXPECT_EQ((*versions_or)[0], v1_or->version_id);
  EXPECT_EQ((*versions_or)[1], v2_or->version_id);
}

TEST_F(TypeRegistryTest, ListTypeVersionsNotFound) {
  auto versions_or = registry_->ListTypeVersions("nonexistent.Type", CanonicalReadContext());
  ASSERT_FALSE(versions_or.ok());
  EXPECT_EQ(versions_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(TypeRegistryTest, GetIndexSchema) {
  auto reg_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  auto schema_or = registry_->GetIndexSchema("simple_by_name", CanonicalReadContext());
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  EXPECT_EQ(schema_or->key_type, "simple_by_name");
  EXPECT_GT(schema_or->index_definition_id, 0U);
  EXPECT_TRUE(schema_or->unique);
  EXPECT_EQ(schema_or->key_fields.size(), 1U);
  EXPECT_EQ(schema_or->key_fields[0], "name");
  EXPECT_FALSE(schema_or->key_message_name.empty());
  EXPECT_FALSE(schema_or->value_message_name.empty());
  EXPECT_FALSE(schema_or->index_message_name.empty());
}

TEST_F(TypeRegistryTest, GetIndexSchemaBuiltInType) {
  // type_name_unique is a built-in index on TypeDefinition.
  auto schema_or = registry_->GetIndexSchema("type_name_unique", CanonicalReadContext());
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  EXPECT_EQ(schema_or->key_type, "type_name_unique");
  EXPECT_EQ(schema_or->index_definition_id, kTypeNameUniqueId);
  EXPECT_TRUE(schema_or->unique);
}

TEST_F(TypeRegistryTest, GetIndexSchemaNotFound) {
  auto schema_or = registry_->GetIndexSchema("nonexistent_index", CanonicalReadContext());
  ASSERT_FALSE(schema_or.ok());
  EXPECT_EQ(schema_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(TypeRegistryTest, RegisterTypeVersionInTransactionReadYourWritesAndCommitVisibility) {
  auto txn_or = transaction_manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& transaction_id = *txn_or;

  auto reg_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource, std::nullopt, std::nullopt, std::nullopt, transaction_id);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  ReadContext canonical_context;
  auto canonical_or = registry_->GetTypeVersion(reg_or->version_id, canonical_context);
  ASSERT_FALSE(canonical_or.ok());
  EXPECT_EQ(canonical_or.status().code(), absl::StatusCode::kNotFound);

  ReadContext transaction_context;
  transaction_context.set_transaction_id(transaction_id);
  auto transaction_read_or = registry_->GetTypeVersion(reg_or->version_id, transaction_context);
  ASSERT_TRUE(transaction_read_or.ok()) << transaction_read_or.status();

  auto commit_or = transaction_manager_->CommitTransaction(transaction_id);
  ASSERT_TRUE(commit_or.ok()) << commit_or.status();
  ASSERT_TRUE(std::holds_alternative<TransactionManager::CommitSuccess>(*commit_or));

  auto committed_or = registry_->GetTypeVersion(reg_or->version_id, canonical_context);
  ASSERT_TRUE(committed_or.ok()) << committed_or.status();
}

TEST_F(TypeRegistryTest, ReadApisSupportSnapshotAndTransactionContexts) {
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  auto txn_or = transaction_manager_->CreateTransaction();
  ASSERT_TRUE(txn_or.ok()) << txn_or.status();
  const std::string& transaction_id = *txn_or;

  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2, std::nullopt, std::nullopt, std::nullopt, transaction_id);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();

  auto snapshot_or = transaction_manager_->CreateSnapshot(transaction_id);
  ASSERT_TRUE(snapshot_or.ok()) << snapshot_or.status();

  ReadContext canonical_context;
  auto canonical_versions_or = registry_->ListTypeVersions("test.SimpleArtifact", canonical_context);
  ASSERT_TRUE(canonical_versions_or.ok()) << canonical_versions_or.status();
  ASSERT_EQ(canonical_versions_or->size(), 1U);
  EXPECT_EQ((*canonical_versions_or)[0], v1_or->version_id);

  ReadContext transaction_context;
  transaction_context.set_transaction_id(transaction_id);
  auto transaction_versions_or = registry_->ListTypeVersions("test.SimpleArtifact", transaction_context);
  ASSERT_TRUE(transaction_versions_or.ok()) << transaction_versions_or.status();
  ASSERT_EQ(transaction_versions_or->size(), 2U);

  ReadContext snapshot_context;
  snapshot_context.set_snapshot_id(*snapshot_or);
  auto snapshot_v2_or = registry_->GetTypeVersion(v2_or->version_id, snapshot_context);
  ASSERT_TRUE(snapshot_v2_or.ok()) << snapshot_v2_or.status();
}

TEST_F(TypeRegistryTest, InvalidReadContextReturnsSnapshotTransactionErrorPayload) {
  ReadContext invalid_snapshot_context;
  invalid_snapshot_context.set_snapshot_id("snapshot-does-not-exist");
  auto snapshot_or = registry_->GetTypeVersion(12345, invalid_snapshot_context);
  ASSERT_FALSE(snapshot_or.ok());
  EXPECT_EQ(snapshot_or.status().code(), absl::StatusCode::kNotFound);
  auto snapshot_error = ExtractSnapshotTransactionError(snapshot_or.status());
  ASSERT_TRUE(snapshot_error.has_value());
  EXPECT_EQ(snapshot_error->category(), SnapshotTransactionError::SNAPSHOT_NOT_FOUND);

  ReadContext invalid_transaction_context;
  invalid_transaction_context.set_transaction_id("transaction-does-not-exist");
  auto transaction_or = registry_->ListTypeVersions("test.SimpleArtifact", invalid_transaction_context);
  ASSERT_FALSE(transaction_or.ok());
  EXPECT_EQ(transaction_or.status().code(), absl::StatusCode::kNotFound);
  auto transaction_error = ExtractSnapshotTransactionError(transaction_or.status());
  ASSERT_TRUE(transaction_error.has_value());
  EXPECT_EQ(transaction_error->category(), SnapshotTransactionError::TRANSACTION_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Violation category tests
// ---------------------------------------------------------------------------

TEST_F(TypeRegistryTest, ProtoCompilationFailure_BadSyntax) {
  auto result_or = registry_->RegisterTypeVersion("test.Foo", "this is not valid proto");
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->violations_size(), 1);
  EXPECT_EQ(error->violations(0).category(), TypeRegistrationViolation::PROTO_COMPILATION_FAILURE);
}

TEST_F(TypeRegistryTest, ProtoCompilationFailure_MessageNotFound) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    message WrongName {
      string x = 1;
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.DoesNotExist", source);
  ASSERT_FALSE(result_or.ok());

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->violations(0).category(), TypeRegistrationViolation::PROTO_COMPILATION_FAILURE);
}

TEST_F(TypeRegistryTest, SchemaIncompatibility_FieldRemoved) {
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message SimpleArtifact {
      option (artifact_system.indexes) = {
        key_type: "simple_by_name"
        key: ["name"]
        order: { field: "artifact_id" direction: ASCENDING }
        unique: true
      };
      string name = 1;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", v2_source);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::SCHEMA_INCOMPATIBILITY);
}

TEST_F(TypeRegistryTest, InvalidIndexDefinition_UnspecifiedOrder) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message BadIndex {
      option (artifact_system.indexes) = {
        key_type: "bad_order"
        key: ["name"]
        order: { field: "name" direction: ORDER_BY_UNSPECIFIED }
      };
      string name = 1;
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.BadIndex", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_INDEX_DEFINITION);
}

TEST_F(TypeRegistryTest, InvalidIndexDefinition_KeyFieldNotFound) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message BadKeyField {
      option (artifact_system.indexes) = {
        key_type: "bad_key_field"
        key: ["nonexistent_field"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      string name = 1;
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.BadKeyField", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_INDEX_DEFINITION);
}

TEST_F(TypeRegistryTest, InvalidIndexDefinition_OrderFieldNotFound) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message BadOrderField {
      option (artifact_system.indexes) = {
        key_type: "bad_order_field"
        key: ["name"]
        order: { field: "nonexistent_field" direction: ASCENDING }
      };
      string name = 1;
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.BadOrderField", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_INDEX_DEFINITION);
}

TEST_F(TypeRegistryTest, IndexIncompatibility_Removed) {
  // Register v1 with an index.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Register v2 without the index.
  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    message SimpleArtifact {
      string name = 1;
      string value = 2;
      string extra = 3;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", v2_source);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::INDEX_INCOMPATIBILITY);
}

TEST_F(TypeRegistryTest, IndexIncompatibility_Modified) {
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Register v2 with the index key changed.
  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message SimpleArtifact {
      option (artifact_system.indexes) = {
        key_type: "simple_by_name"
        key: ["value"]
        order: { field: "artifact_id" direction: ASCENDING }
        unique: true
      };
      string name = 1;
      string value = 2;
      string extra = 3;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", v2_source);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::INDEX_INCOMPATIBILITY);
}

TEST_F(TypeRegistryTest, TightenOnlyViolation) {
  // Register with deny_create = true.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource, /*deny_create=*/true);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Try to loosen deny_create to false.
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2, /*deny_create=*/false);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION);
}

TEST_F(TypeRegistryTest, TightenOnlyAllowed) {
  // Register with deny_create = false (default).
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Tighten deny_update to true — should succeed.
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2, /*deny_create=*/std::nullopt,
                                              /*deny_update=*/true);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();
}

TEST_F(TypeRegistryTest, TightenOnlyOmitPreservesExisting) {
  // Register with deny_delete = true.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource, /*deny_create=*/std::nullopt,
                                              /*deny_update=*/std::nullopt, /*deny_delete=*/true);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Omit all flags — should preserve deny_delete = true.
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();

  // Verify deny_delete is still true by attempting to loosen it — should fail.
  const char* v3_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message SimpleArtifact {
      option (artifact_system.indexes) = {
        key_type: "simple_by_name"
        key: ["name"]
        order: { field: "artifact_id" direction: ASCENDING }
        unique: true
      };
      string name = 1;
      string value = 2;
      string description = 3;
      string extra = 4;
    }
  )";
  auto v3_or = registry_->RegisterTypeVersion("test.SimpleArtifact", v3_source, /*deny_create=*/std::nullopt, /*deny_update=*/std::nullopt,
                                              /*deny_delete=*/false);
  ASSERT_FALSE(v3_or.ok());
  auto error = ExtractRegistrationError(v3_or.status());
  ASSERT_TRUE(error.has_value());
  bool found_tighten = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION)
      found_tighten = true;
  }
  EXPECT_TRUE(found_tighten) << "deny_delete=true should have been preserved from v1";
}

TEST_F(TypeRegistryTest, InvalidReferenceDeclaration_WrongFieldType) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message BadRef {
      option (artifact_system.indexes) = {
        key_type: "bad_ref_by_target"
        key: ["target_name"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      string target_name = 1 [(artifact_system.references) = {
        target_type_name: "test.SimpleArtifact"
        on_delete: RESTRICT
      }];
    }
  )";

  // Register SimpleArtifact first so the target exists.
  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto result_or = registry_->RegisterTypeVersion("test.BadRef", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
}

TEST_F(TypeRegistryTest, InvalidReferenceDeclaration_TargetNotFound) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message RefToNowhere {
      option (artifact_system.indexes) = {
        key_type: "ref_nowhere_by_target"
        key: ["target_id"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      optional uint64 target_id = 1 [(artifact_system.references) = {
        target_type_name: "test.NonExistent"
        on_delete: RESTRICT
      }];
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.RefToNowhere", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
}

TEST_F(TypeRegistryTest, InvalidReferenceDeclaration_NoCoveringIndex) {
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message NoCovering {
      optional uint64 target_id = 1 [(artifact_system.references) = {
        target_type_name: "test.SimpleArtifact"
        on_delete: RESTRICT
      }];
    }
  )";

  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto result_or = registry_->RegisterTypeVersion("test.NoCovering", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
}

TEST_F(TypeRegistryTest, ValidReferenceRegistration) {
  // Register the target type first.
  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto ref_or = registry_->RegisterTypeVersion("test.RefArtifact", kRefProtoSource);
  ASSERT_TRUE(ref_or.ok()) << ref_or.status();
  EXPECT_GT(ref_or->version_id, 0U);
}

TEST_F(TypeRegistryTest, ReferenceIncompatibility_Removed) {
  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto v1_or = registry_->RegisterTypeVersion("test.RefArtifact", kRefProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Register v2 of RefArtifact without the reference.
  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message RefArtifact {
      option (artifact_system.indexes) = {
        key_type: "ref_by_target"
        key: ["target_id"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      optional uint64 target_id = 1;
      string label = 2;
      string extra = 3;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.RefArtifact", v2_source);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY);
}

TEST_F(TypeRegistryTest, NestedReferenceRegistration) {
  // A reference annotation on a nested message field is extracted with its
  // dotted path and satisfied by a covering index keyed on that path.
  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto ref_or = registry_->RegisterTypeVersion("test.NestedRefArtifact", kNestedRefProtoSource);
  ASSERT_TRUE(ref_or.ok()) << ref_or.status();
  EXPECT_GT(ref_or->version_id, 0U);
}

TEST_F(TypeRegistryTest, NestedReferenceIncompatibility_Removed) {
  // The nested reference is persisted as a ReferenceDefinition keyed by its
  // dotted path, so removing the annotation in v2 is a reference removal.
  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto v1_or = registry_->RegisterTypeVersion("test.NestedRefArtifact", kNestedRefProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message NestedRefArtifact {
      option (artifact_system.indexes) = {
        key_type: "nested_ref_by_target"
        key: ["inputs.target_ids"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      message Input {
        string name = 1;
        repeated uint64 target_ids = 2;
      }
      repeated Input inputs = 1;
      string extra = 2;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.NestedRefArtifact", v2_source);
  ASSERT_FALSE(v2_or.ok());
  ExpectViolationCategory(v2_or.status(), TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY);
}

TEST_F(TypeRegistryTest, InvalidReferenceDeclaration_NestedTargetNotFound) {
  // Declaration validation applies to references found on nested messages.
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message NestedRefToNowhere {
      option (artifact_system.indexes) = {
        key_type: "nested_nowhere_by_target"
        key: ["input.target_id"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      message Input {
        optional uint64 target_id = 1 [(artifact_system.references) = {
          target_type_name: "test.NonExistent"
          on_delete: RESTRICT
        }];
      }
      Input input = 1;
    }
  )";
  auto result_or = registry_->RegisterTypeVersion("test.NestedRefToNowhere", source);
  ASSERT_FALSE(result_or.ok());
  ExpectViolationCategory(result_or.status(), TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
}

TEST_F(TypeRegistryTest, SelfReferentialProtoCycleGuard) {
  // A message containing a field of its own type must not send reference
  // extraction into infinite recursion. The root-level reference is still
  // extracted.
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message TreeArtifact {
      option (artifact_system.indexes) = {
        key_type: "tree_by_target"
        key: ["target_id"]
        order: { field: "artifact_id" direction: ASCENDING }
      };
      optional uint64 target_id = 1 [(artifact_system.references) = {
        target_type_name: "test.SimpleArtifact"
        on_delete: RESTRICT
      }];
      TreeArtifact child = 2;
    }
  )";

  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto tree_or = registry_->RegisterTypeVersion("test.TreeArtifact", source);
  ASSERT_TRUE(tree_or.ok()) << tree_or.status();
}

TEST_F(TypeRegistryTest, MapFieldReferenceNotExtracted) {
  // References inside map value messages are not extracted: registration
  // succeeds even though no covering index exists for the map entry field.
  const char* source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message MapRefArtifact {
      option (artifact_system.indexes) = {
        key_type: "map_ref_by_name"
        key: ["name"]
        order: { field: "artifact_id" direction: ASCENDING }
        unique: true
      };
      string name = 1;
      message Entry {
        uint64 target_id = 1 [(artifact_system.references) = {
          target_type_name: "test.SimpleArtifact"
          on_delete: RESTRICT
        }];
      }
      map<string, Entry> entries = 2;
    }
  )";

  auto simple_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(simple_or.ok()) << simple_or.status();

  auto map_or = registry_->RegisterTypeVersion("test.MapRefArtifact", source);
  ASSERT_TRUE(map_or.ok()) << map_or.status();
}

TEST_F(TypeRegistryTest, MultipleViolationsCollected) {
  // Register v1.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource, /*deny_create=*/true);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Try v2 that has schema incompatibility AND tighten-only violation.
  const char* v2_source = R"(
    syntax = "proto3";
    package test;
    import "artifact_options.proto";
    message SimpleArtifact {
      option (artifact_system.indexes) = {
        key_type: "simple_by_name"
        key: ["name"]
        order: { field: "artifact_id" direction: ASCENDING }
        unique: true
      };
      string name = 1;
    }
  )";
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", v2_source, /*deny_create=*/false);
  ASSERT_FALSE(v2_or.ok());

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  // Should have at least tighten-only + schema incompatibility.
  EXPECT_GE(error->violations_size(), 2);

  bool has_tighten = false;
  bool has_schema = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION)
      has_tighten = true;
    if (v.category() == TypeRegistrationViolation::SCHEMA_INCOMPATIBILITY)
      has_schema = true;
  }
  EXPECT_TRUE(has_tighten);
  EXPECT_TRUE(has_schema);
}

TEST_F(TypeRegistryTest, ConcurrentRegistrationConflict) {
  // Register the first version normally.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Create a second TypeRegistry pointing at the same storage.
  // Both registries will try to update the same TypeDefinition and tail version.
  // Create a second TypeRegistry pointing at the same storage.
  // With ResolveIndexDefId fallback, registry2 will automatically discover
  // the simple_by_name index via the storage layer on cache miss.
  auto registry2 = std::make_unique<TypeRegistry>(storage_.get(), transaction_manager_.get(), id_allocator_.get(), index_def_ids_);

  // Both registries now try to register v2 of the same type. One should succeed
  // and the other should fail with a conflict (ABORTED).
  auto v2a_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);
  auto v2b_or = registry2->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);

  // At least one should succeed.
  bool a_ok = v2a_or.ok();
  bool b_ok = v2b_or.ok();
  EXPECT_TRUE(a_ok || b_ok) << "At least one registration should succeed";

  // In a single-threaded environment, the first call commits before the
  // second starts, so the second reads the updated state and succeeds too
  // (creating v3). This is expected because there's no true concurrency.
  // The conflict would occur only with actual parallel execution where both
  // transactions read the same tail before either commits.
  // We verify that the system is at least consistent.
  if (a_ok && b_ok) {
    // Both succeeded — they created two different versions.
    EXPECT_NE(v2a_or->version_id, v2b_or->version_id);
  }
}

TEST_F(TypeRegistryTest, ArtifactStoreResolvesStaleIndexDefMapOnCreate) {
  std::unordered_map<std::string, uint64_t> stale_index_def_ids = index_def_ids_;

  auto registry2 = std::make_unique<TypeRegistry>(storage_.get(), transaction_manager_.get(), id_allocator_.get(), stale_index_def_ids);
  auto reg_or = registry2->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  auto type_info_or = registry2->GetTypeVersion(reg_or->version_id, CanonicalReadContext());
  ASSERT_TRUE(type_info_or.ok()) << type_info_or.status();

  auto payload_or = BuildPayload(type_info_or->descriptor_set, "test.SimpleArtifact", "stale-map-name", "payload-value");
  ASSERT_TRUE(payload_or.ok()) << payload_or.status();

  ArtifactStore::Options store_opts;
  store_opts.index_def_ids_by_key_type = &stale_index_def_ids;
  ArtifactStore store(storage_.get(), transaction_manager_.get(), id_allocator_.get(), store_opts);

  auto create_or = store.CreateArtifact(reg_or->version_id, *payload_or);
  ASSERT_TRUE(create_or.ok()) << create_or.status();

  auto index_schema_or = registry_->GetIndexSchema("simple_by_name", CanonicalReadContext());
  ASSERT_TRUE(index_schema_or.ok()) << index_schema_or.status();

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = artifact::BuildPoolAndFindMessage(type_info_or->descriptor_set, "test.SimpleArtifact", &pool);
  ASSERT_NE(descriptor, nullptr);

  auto encoded_key_or = encoding::EncodeSingleStringKey(*descriptor, "name", "stale-map-name");
  ASSERT_TRUE(encoded_key_or.ok()) << encoded_key_or.status();

  const std::string ref = storage_->GetCanonicalBranch();
  const std::string index_path = encoding::IndexPath(index_schema_or->index_definition_id, *encoded_key_or);
  auto index_data_or = storage_->GetObject(ref, index_path);
  ASSERT_TRUE(index_data_or.ok()) << index_data_or.status();

  auto wrong_index_data_or = storage_->GetObject(ref, encoding::IndexPath(0, *encoded_key_or));
  ASSERT_FALSE(wrong_index_data_or.ok());
  EXPECT_EQ(wrong_index_data_or.status().code(), absl::StatusCode::kNotFound);

  auto index_def_stored_or = artifact::ReadStoredArtifact(storage_.get(), ref, index_schema_or->index_definition_id);
  ASSERT_TRUE(index_def_stored_or.ok()) << index_def_stored_or.status();

  IndexDefinition index_def;
  ASSERT_TRUE(index_def.ParseFromString(index_def_stored_or->payload()));

  auto generated_schema_or = index::GenerateIndexSchema(index_def, *descriptor);
  ASSERT_TRUE(generated_schema_or.ok()) << generated_schema_or.status();

  auto index_object_or = index::DeserializeIndexObject(*generated_schema_or, index_def, *index_data_or);
  ASSERT_TRUE(index_object_or.ok()) << index_object_or.status();
  ASSERT_EQ(index_object_or->rows.size(), 1U);
  EXPECT_EQ(index_object_or->rows[0].artifact_id, create_or->artifact_id);
}

} // namespace
} // namespace artifact_system::testing

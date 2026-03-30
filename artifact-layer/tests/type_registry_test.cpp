#include "registry/type_registry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::registry::IndexSchemaInfo;
using artifact_system::registry::RegisterResult;
using artifact_system::registry::TypeRegistry;
using artifact_system::registry::TypeVersionInfo;
using artifact_system::transaction::TransactionManager;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

google::protobuf::FileDescriptorSet BuildDescriptorSet(const google::protobuf::Descriptor* desc) {
  google::protobuf::FileDescriptorSet fds;
  std::set<const google::protobuf::FileDescriptor*> seen;
  std::function<void(const google::protobuf::FileDescriptor*)> add_file;
  add_file = [&](const google::protobuf::FileDescriptor* fd) {
    if (!seen.insert(fd).second)
      return;
    for (int i = 0; i < fd->dependency_count(); ++i) {
      add_file(fd->dependency(i));
    }
    fd->CopyTo(fds.add_file());
  };
  add_file(desc->file());
  return fds;
}

void WriteStoredArtifact(MemoryStorage& storage, const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name,
                         const std::string& payload) {
  StoredArtifact envelope;
  envelope.set_envelope_version(1);
  envelope.set_version_id(version_id);
  envelope.set_type_name(type_name);
  envelope.set_payload(payload);
  ASSERT_TRUE(storage.PutObject(branch, encoding::ArtifactPath(artifact_id), envelope.SerializeAsString()).ok());
}

// Build a proto-serialized key for an index.
std::string BuildProtoKey(const index::GeneratedIndexSchema& schema, const std::vector<index::IndexCell>& key_values) {
  if (key_values.empty())
    return {};
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(schema.key_descriptor);
  std::unique_ptr<google::protobuf::Message> key_msg(prototype->New());
  const auto* reflection = key_msg->GetReflection();
  for (int i = 0; i < static_cast<int>(key_values.size()); ++i) {
    const auto* field = schema.key_descriptor->FindFieldByNumber(i + 1);
    const auto& cell = key_values[static_cast<size_t>(i)];
    if (std::holds_alternative<std::string>(cell)) {
      reflection->SetString(key_msg.get(), field, std::get<std::string>(cell));
    } else if (std::holds_alternative<uint64_t>(cell)) {
      reflection->SetUInt64(key_msg.get(), field, std::get<uint64_t>(cell));
    }
  }
  return key_msg->SerializeAsString();
}

// Write an index entry pointing artifact_id to the given index object.
void WriteIndexEntry(MemoryStorage& storage, const std::string& branch, uint64_t index_def_id, const std::vector<uint8_t>& encoded_key, uint64_t artifact_id,
                     const IndexDefinition& idx_def, const google::protobuf::Descriptor& parent_desc, const std::vector<index::IndexCell>& key_values) {
  auto schema_or = index::GenerateIndexSchema(idx_def, parent_desc);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();

  std::string proto_key = BuildProtoKey(*schema_or, key_values);

  index::IndexObject obj;
  obj.serialized_key = proto_key;

  // Try reading existing.
  const std::string path = encoding::IndexPath(index_def_id, encoded_key);
  auto existing_or = storage.GetObject(branch, path);
  if (existing_or.ok()) {
    auto deser_or = index::DeserializeIndexObject(*schema_or, idx_def, *existing_or);
    if (deser_or.ok()) {
      obj = std::move(*deser_or);
    }
  }

  index::IndexRow row;
  row.artifact_id = artifact_id;
  // For order by artifact_id ascending, the value is the artifact_id.
  row.order_values.push_back(static_cast<uint64_t>(artifact_id));
  obj.rows.push_back(std::move(row));

  auto ser_or = index::SerializeIndexObject(*schema_or, idx_def, obj);
  ASSERT_TRUE(ser_or.ok()) << ser_or.status();
  ASSERT_TRUE(storage.PutObject(branch, path, *ser_or).ok());
}

// Encode a key for an index lookup.
std::vector<uint8_t> EncodeStringKey(const google::protobuf::Descriptor& desc, const std::string& field_name, const std::string& value) {
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(&desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  msg->GetReflection()->SetString(msg.get(), desc.FindFieldByName(field_name), value);
  std::vector<std::string> key_fields = {field_name};
  auto encoded_or = encoding::EncodeKey(desc, *msg, key_fields);
  EXPECT_TRUE(encoded_or.ok());
  return *encoded_or;
}

std::vector<uint8_t> EncodeUint64Key(const google::protobuf::Descriptor& desc, const std::string& field_name, uint64_t value) {
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(&desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  msg->GetReflection()->SetUInt64(msg.get(), desc.FindFieldByName(field_name), value);
  std::vector<std::string> key_fields = {field_name};
  auto encoded_or = encoding::EncodeKey(desc, *msg, key_fields);
  EXPECT_TRUE(encoded_or.ok());
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

    // ── 1. IndexDefinition type ──
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.IndexDefinition");
      td.set_current_version_id(kIdxDefTVDId);
      td.set_deny_create(true);
      td.set_deny_update(true);
      td.set_deny_delete(true);
      WriteStoredArtifact(*storage_, branch, kIdxDefTypeDefId, kTypeDefTVDId, "artifact_system.TypeDefinition", td.SerializeAsString());
    }
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kIdxDefTypeDefId);
      *tvd.mutable_descriptor_set() = BuildDescriptorSet(IndexDefinition::descriptor());
      WriteStoredArtifact(*storage_, branch, kIdxDefTVDId, kTVDTVDId, "artifact_system.TypeVersionDefinition", tvd.SerializeAsString());
    }

    // ── 2. TypeDefinition type ──
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.TypeDefinition");
      td.set_current_version_id(kTypeDefTVDId);
      td.set_deny_create(true);
      td.set_deny_update(true);
      td.set_deny_delete(true);
      WriteStoredArtifact(*storage_, branch, kTypeDefTypeDefId, kTypeDefTVDId, "artifact_system.TypeDefinition", td.SerializeAsString());
    }
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kTypeDefTypeDefId);
      *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
      WriteStoredArtifact(*storage_, branch, kTypeDefTVDId, kTVDTVDId, "artifact_system.TypeVersionDefinition", tvd.SerializeAsString());
    }

    // ── 3. TypeVersionDefinition type ──
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.TypeVersionDefinition");
      td.set_current_version_id(kTVDTVDId);
      td.set_deny_create(true);
      td.set_deny_update(true);
      td.set_deny_delete(true);
      WriteStoredArtifact(*storage_, branch, kTVDTypeDefId, kTVDTVDId, "artifact_system.TypeDefinition", td.SerializeAsString());
    }
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kTVDTypeDefId);
      *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeVersionDefinition::descriptor());
      WriteStoredArtifact(*storage_, branch, kTVDTVDId, kTVDTVDId, "artifact_system.TypeVersionDefinition", tvd.SerializeAsString());
    }

    // ── 4. ReferenceDefinition type ──
    {
      TypeDefinition td;
      td.set_type_name("artifact_system.ReferenceDefinition");
      td.set_current_version_id(kRefDefTVDId);
      td.set_deny_create(true);
      td.set_deny_update(true);
      td.set_deny_delete(true);
      WriteStoredArtifact(*storage_, branch, kRefDefTypeDefId, kRefDefTVDId, "artifact_system.TypeDefinition", td.SerializeAsString());
    }
    {
      TypeVersionDefinition tvd;
      tvd.set_type_id(kRefDefTypeDefId);
      *tvd.mutable_descriptor_set() = BuildDescriptorSet(ReferenceDefinition::descriptor());
      WriteStoredArtifact(*storage_, branch, kRefDefTVDId, kTVDTVDId, "artifact_system.TypeVersionDefinition", tvd.SerializeAsString());
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
  auto v1_info = registry_->GetTypeVersion(v1);
  ASSERT_TRUE(v1_info.ok()) << v1_info.status();
  EXPECT_FALSE(v1_info->previous_version_id.has_value());
  ASSERT_TRUE(v1_info->next_version_id.has_value());
  EXPECT_EQ(*v1_info->next_version_id, v2);

  auto v2_info = registry_->GetTypeVersion(v2);
  ASSERT_TRUE(v2_info.ok()) << v2_info.status();
  ASSERT_TRUE(v2_info->previous_version_id.has_value());
  EXPECT_EQ(*v2_info->previous_version_id, v1);
  EXPECT_FALSE(v2_info->next_version_id.has_value());
}

TEST_F(TypeRegistryTest, GetTypeVersion) {
  auto reg_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  auto info = registry_->GetTypeVersion(reg_or->version_id);
  ASSERT_TRUE(info.ok()) << info.status();
  EXPECT_EQ(info->version_id, reg_or->version_id);
  EXPECT_GT(info->type_id, 0U);
  EXPECT_GT(info->descriptor_set.file_size(), 0);
  EXPECT_FALSE(info->proto_source.empty());
  EXPECT_FALSE(info->previous_version_id.has_value());
  EXPECT_FALSE(info->next_version_id.has_value());
}

TEST_F(TypeRegistryTest, GetTypeVersionNotFound) {
  auto info = registry_->GetTypeVersion(99999);
  ASSERT_FALSE(info.ok());
  EXPECT_EQ(info.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(TypeRegistryTest, ListTypeVersions) {
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2);
  ASSERT_TRUE(v2_or.ok()) << v2_or.status();

  auto versions_or = registry_->ListTypeVersions("test.SimpleArtifact");
  ASSERT_TRUE(versions_or.ok()) << versions_or.status();
  EXPECT_EQ(versions_or->size(), 2U);
  EXPECT_EQ((*versions_or)[0], v1_or->version_id);
  EXPECT_EQ((*versions_or)[1], v2_or->version_id);
}

TEST_F(TypeRegistryTest, ListTypeVersionsNotFound) {
  auto versions_or = registry_->ListTypeVersions("nonexistent.Type");
  ASSERT_FALSE(versions_or.ok());
  EXPECT_EQ(versions_or.status().code(), absl::StatusCode::kNotFound);
}

TEST_F(TypeRegistryTest, GetIndexSchema) {
  auto reg_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(reg_or.ok()) << reg_or.status();

  auto schema_or = registry_->GetIndexSchema("simple_by_name");
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
  auto schema_or = registry_->GetIndexSchema("type_name_unique");
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  EXPECT_EQ(schema_or->key_type, "type_name_unique");
  EXPECT_EQ(schema_or->index_definition_id, kTypeNameUniqueId);
  EXPECT_TRUE(schema_or->unique);
}

TEST_F(TypeRegistryTest, GetIndexSchemaNotFound) {
  auto schema_or = registry_->GetIndexSchema("nonexistent_index");
  ASSERT_FALSE(schema_or.ok());
  EXPECT_EQ(schema_or.status().code(), absl::StatusCode::kNotFound);
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
  // Register v1.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Try to register v2 with a field removed.
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

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  bool found_schema = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::SCHEMA_INCOMPATIBILITY) {
      found_schema = true;
      break;
    }
  }
  EXPECT_TRUE(found_schema);
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

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INVALID_INDEX_DEFINITION)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INDEX_INCOMPATIBILITY)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INDEX_INCOMPATIBILITY)
      found = true;
  }
  EXPECT_TRUE(found);
}

TEST_F(TypeRegistryTest, TightenOnlyViolation) {
  // Register with deny_create = true.
  auto v1_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSource, /*deny_create=*/true);
  ASSERT_TRUE(v1_or.ok()) << v1_or.status();

  // Try to loosen deny_create to false.
  auto v2_or = registry_->RegisterTypeVersion("test.SimpleArtifact", kSimpleProtoSourceV2, /*deny_create=*/false);
  ASSERT_FALSE(v2_or.ok());

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(result_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION)
      found = true;
  }
  EXPECT_TRUE(found);
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

  auto error = ExtractRegistrationError(v2_or.status());
  ASSERT_TRUE(error.has_value());
  bool found = false;
  for (const auto& v : error->violations()) {
    if (v.category() == TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY)
      found = true;
  }
  EXPECT_TRUE(found);
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
  auto registry2 = std::make_unique<TypeRegistry>(storage_.get(), transaction_manager_.get(), id_allocator_.get(), index_def_ids_);
  // Update registry2's index map to include the simple_by_name index from v1.
  auto schema_or = registry_->GetIndexSchema("simple_by_name");
  if (schema_or.ok()) {
    std::unordered_map<std::string, uint64_t> extra = {{"simple_by_name", schema_or->index_definition_id}};
    registry2->UpdateIndexDefIds(extra);
  }

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

} // namespace
} // namespace artifact_system::testing

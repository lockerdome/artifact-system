#include "artifact/referential_integrity.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/text_format.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "registry/proto_compiler.h"
#include "storage/memory_storage.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::artifact::CascadeDelete;
using artifact_system::artifact::DeleteSideEffect;
using artifact_system::artifact::EnforceDeleteIntegrity;
using artifact_system::artifact::RefIntegrityContext;
using artifact_system::artifact::SetNullUpdate;
using artifact_system::artifact::ValidateReferences;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void WriteArtifact(MemoryStorage& storage, uint64_t id, const std::string& type_name, uint64_t version_id, const std::string& payload) {
  StoredArtifact stored;
  stored.set_envelope_version(1);
  stored.set_version_id(version_id);
  stored.set_type_name(type_name);
  stored.set_payload(payload);
  storage.PutObject("main", encoding::ArtifactPath(id), stored.SerializeAsString()).IgnoreError();
}

void WriteTombstone(MemoryStorage& storage, uint64_t id, const std::string& type_name, uint64_t version_id) {
  StoredArtifact stored;
  stored.set_envelope_version(1);
  stored.set_version_id(version_id);
  stored.set_type_name(type_name);
  // payload left empty => tombstone
  storage.PutObject("main", encoding::ArtifactPath(id), stored.SerializeAsString()).IgnoreError();
}

RefIntegrityContext MakeContext(MemoryStorage& storage) {
  return RefIntegrityContext{&storage, "main"};
}

absl::StatusOr<IndexDefinition> FindIndexDefinitionByKeyType(const google::protobuf::Descriptor& descriptor, const std::string& key_type) {
  const auto& options = descriptor.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    const auto& definition = options.GetExtension(artifact_system::indexes, i);
    if (definition.key_type() == key_type) {
      return definition;
    }
  }
  return absl::NotFoundError("index definition not found");
}

// Encode a uint64 as an index key (8 bytes little-endian), matching the
// EncodeUint64Key helper inside referential_integrity.cpp.
std::vector<uint8_t> EncodeUint64Key(uint64_t value) {
  std::vector<uint8_t> out;
  out.reserve(8);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
  }
  return out;
}

// Encode a string value as an index key (varint length + bytes), matching the
// EncodeStringKey helper inside referential_integrity.cpp.
std::vector<uint8_t> EncodeStringKey(const std::string& value) {
  auto length_bytes = encoding::EncodeVarint(value.size());
  std::vector<uint8_t> out;
  out.reserve(length_bytes.size() + value.size());
  out.insert(out.end(), length_bytes.begin(), length_bytes.end());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

// Build a protobuf-serialized key for an index, using the generated schema.
// The key fields are set from the provided message via EncodeKey, then the
// generated key message is populated and serialized.
absl::StatusOr<std::string> BuildSerializedKey(const IndexDefinition& definition, const google::protobuf::Descriptor& descriptor,
                                               const google::protobuf::Message& source_message) {
  auto schema_or = index::GenerateIndexSchema(definition, descriptor);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  // Encode the key fields from the source message.
  std::vector<std::string> key_fields(definition.key().begin(), definition.key().end());
  auto encoded_key_or = encoding::EncodeKey(descriptor, source_message, key_fields);
  if (!encoded_key_or.ok()) {
    return encoded_key_or.status();
  }

  // Build the key message using the generated schema.
  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* key_proto = factory.GetPrototype(schema_or->key_descriptor);
  std::unique_ptr<google::protobuf::Message> key_message(key_proto->New());

  // Set each key field on the generated key message by reading from the source.
  const google::protobuf::Reflection* source_reflection = source_message.GetReflection();
  const google::protobuf::Reflection* key_reflection = key_message->GetReflection();
  for (int i = 0; i < definition.key_size(); ++i) {
    const auto* source_field = descriptor.FindFieldByName(definition.key(i));
    const auto* key_field = schema_or->key_descriptor->FindFieldByNumber(i + 1);
    if (source_field == nullptr || key_field == nullptr) {
      return absl::InternalError("key field not found");
    }

    switch (source_field->type()) {
    case google::protobuf::FieldDescriptor::TYPE_STRING:
      key_reflection->SetString(key_message.get(), key_field, source_reflection->GetString(source_message, source_field));
      break;
    case google::protobuf::FieldDescriptor::TYPE_UINT64:
    case google::protobuf::FieldDescriptor::TYPE_FIXED64:
      key_reflection->SetUInt64(key_message.get(), key_field, source_reflection->GetUInt64(source_message, source_field));
      break;
    case google::protobuf::FieldDescriptor::TYPE_INT64:
    case google::protobuf::FieldDescriptor::TYPE_SINT64:
    case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
      key_reflection->SetInt64(key_message.get(), key_field, source_reflection->GetInt64(source_message, source_field));
      break;
    case google::protobuf::FieldDescriptor::TYPE_UINT32:
    case google::protobuf::FieldDescriptor::TYPE_FIXED32:
      key_reflection->SetUInt32(key_message.get(), key_field, source_reflection->GetUInt32(source_message, source_field));
      break;
    case google::protobuf::FieldDescriptor::TYPE_INT32:
    case google::protobuf::FieldDescriptor::TYPE_SINT32:
    case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
      key_reflection->SetInt32(key_message.get(), key_field, source_reflection->GetInt32(source_message, source_field));
      break;
    case google::protobuf::FieldDescriptor::TYPE_BOOL:
      key_reflection->SetBool(key_message.get(), key_field, source_reflection->GetBool(source_message, source_field));
      break;
    default:
      return absl::InvalidArgumentError("unsupported key field type");
    }
  }

  return key_message->SerializeAsString();
}

// Build and serialize an index object containing the given artifact IDs.
absl::StatusOr<std::string> BuildIndexObjectBytes(const IndexDefinition& definition, const google::protobuf::Descriptor& descriptor,
                                                  const std::string& serialized_key, const std::vector<uint64_t>& artifact_ids) {
  auto schema_or = index::GenerateIndexSchema(definition, descriptor);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  index::IndexObject object;
  object.serialized_key = serialized_key;
  for (uint64_t id : artifact_ids) {
    index::IndexRow row;
    row.artifact_id = id;
    // The order field is artifact_id (the only order field).
    row.order_values.push_back(id);
    object.rows.push_back(row);
  }

  return index::SerializeIndexObject(*schema_or, definition, object);
}

// Build a FileDescriptorSet covering the given message descriptor and all its
// transitive file dependencies.
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

// Compile a .proto source string and return the descriptor for `type_name`.
// The returned descriptor is owned by `pool`, which must be backed by the
// generated pool so artifact_options.proto extensions resolve.
const google::protobuf::Descriptor* CompileProtoSource(const std::string& proto_source, const std::string& type_name, google::protobuf::DescriptorPool& pool) {
  registry::ProtoCompiler compiler;
  auto result = compiler.Compile(proto_source, type_name);
  if (std::holds_alternative<registry::CompilationError>(result)) {
    ADD_FAILURE() << "proto compilation failed: " << std::get<registry::CompilationError>(result).description;
    return nullptr;
  }
  return artifact::BuildPoolAndFindMessage(std::get<registry::CompilationResult>(result).descriptor_set, type_name, &pool);
}

// Proto with reference annotations on nested message fields, for exercising
// recursive reference validation. Inner.target_id is implicit-presence, so a
// buggy descent into an unset parent message would validate the default 0 and
// surface as a REFERENCE_TARGET_NOT_FOUND violation.
constexpr const char* kNestedRefProtoSource = R"(
syntax = "proto3";
package artifact_system.testing;
import "artifact_options.proto";

message NestedRefHolder {
  message Inner {
    uint64 target_id = 1 [(artifact_system.references) = {
      target_type_name: "TypeDefinition"
      on_delete: RESTRICT
    }];
  }

  message Outer {
    Inner inner = 1;
  }

  Inner nested = 1;
  repeated Inner items = 2;
  Outer outer = 3;
  map<string, Inner> entries = 4;
}
)";

// Write a "references_by_target_type" index object to storage.
void WriteReferencesByTargetTypeIndex(MemoryStorage& storage, uint64_t index_def_id, const std::string& target_type_name,
                                      const std::vector<uint64_t>& ref_def_artifact_ids) {
  const auto* ref_def_descriptor = ReferenceDefinition::descriptor();
  auto idx_def_or = FindIndexDefinitionByKeyType(*ref_def_descriptor, "references_by_target_type");
  ASSERT_TRUE(idx_def_or.ok()) << idx_def_or.status();

  // Build the serialized key using the generated schema.
  ReferenceDefinition key_source;
  key_source.set_target_type_name(target_type_name);
  auto serialized_key_or = BuildSerializedKey(*idx_def_or, *ref_def_descriptor, key_source);
  ASSERT_TRUE(serialized_key_or.ok()) << serialized_key_or.status();

  auto bytes_or = BuildIndexObjectBytes(*idx_def_or, *ref_def_descriptor, *serialized_key_or, ref_def_artifact_ids);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();

  // The implementation looks up the index using EncodeStringKey for the path.
  const std::vector<uint8_t> encoded_key = EncodeStringKey(target_type_name);
  const std::string path = encoding::IndexPath(index_def_id, encoded_key);
  ASSERT_TRUE(storage.PutObject("main", path, *bytes_or).ok());
}

// Write a covering index object to storage.
// Uses TypeVersionDefinition::descriptor() for schema generation since the
// covering index key field ("type_id") exists on that descriptor.
void WriteCoveringIndex(MemoryStorage& storage, uint64_t index_def_id, uint64_t target_artifact_id, const std::vector<uint64_t>& referencing_artifact_ids,
                        const std::string& covering_key_type) {
  const auto* tvd_descriptor = TypeVersionDefinition::descriptor();

  IndexDefinition covering_def;
  covering_def.set_key_type(covering_key_type);
  covering_def.add_key("type_id");
  auto* order = covering_def.add_order();
  order->set_field("artifact_id");
  order->set_direction(OrderDefinition::ASCENDING);
  covering_def.set_unique(false);

  // Build a serialized key using TypeVersionDefinition (which has type_id).
  TypeVersionDefinition key_source;
  key_source.set_type_id(target_artifact_id);
  auto serialized_key_or = BuildSerializedKey(covering_def, *tvd_descriptor, key_source);
  ASSERT_TRUE(serialized_key_or.ok()) << serialized_key_or.status();

  auto bytes_or = BuildIndexObjectBytes(covering_def, *tvd_descriptor, *serialized_key_or, referencing_artifact_ids);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();

  // The implementation looks up the index using EncodeUint64Key for the path.
  const std::vector<uint8_t> encoded_key = EncodeUint64Key(target_artifact_id);
  const std::string path = encoding::IndexPath(index_def_id, encoded_key);
  ASSERT_TRUE(storage.PutObject("main", path, *bytes_or).ok());
}

// Write a "type_name_unique" index object to storage.
// Maps type_name -> TypeDefinition artifact_id using TypeDefinition::descriptor().
void WriteTypeNameUniqueIndex(MemoryStorage& storage, uint64_t index_def_id, const std::string& type_name, const std::vector<uint64_t>& type_def_artifact_ids) {
  const auto* td_descriptor = TypeDefinition::descriptor();
  auto idx_def_or = FindIndexDefinitionByKeyType(*td_descriptor, "type_name_unique");
  ASSERT_TRUE(idx_def_or.ok()) << idx_def_or.status();

  TypeDefinition key_source;
  key_source.set_type_name(type_name);
  auto serialized_key_or = BuildSerializedKey(*idx_def_or, *td_descriptor, key_source);
  ASSERT_TRUE(serialized_key_or.ok()) << serialized_key_or.status();

  auto bytes_or = BuildIndexObjectBytes(*idx_def_or, *td_descriptor, *serialized_key_or, type_def_artifact_ids);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();

  const std::vector<uint8_t> encoded_key = EncodeStringKey(type_name);
  const std::string path = encoding::IndexPath(index_def_id, encoded_key);
  ASSERT_TRUE(storage.PutObject("main", path, *bytes_or).ok());
}

// Set up the full referencing type resolution chain needed by
// EnforceDeleteIntegrity's ResolveReferencingType function:
//   type_name_unique index -> TypeDefinition -> TypeVersionDefinition (with descriptor_set)
//
// Parameters:
//   referencing_type_name: the full protobuf name of the referencing type
//   referencing_descriptor: the Descriptor* for the referencing type
//   tnu_index_def_id: artifact ID for the type_name_unique index definition
//   type_def_id: artifact ID for the TypeDefinition artifact
//   type_version_def_id: artifact ID for the TypeVersionDefinition artifact
void SetupReferencingTypeResolution(MemoryStorage& storage, const std::string& referencing_type_name,
                                    const google::protobuf::Descriptor* referencing_descriptor, uint64_t tnu_index_def_id, uint64_t type_def_id,
                                    uint64_t type_version_def_id) {
  // 1. Write the type_name_unique index entry.
  WriteTypeNameUniqueIndex(storage, tnu_index_def_id, referencing_type_name, {type_def_id});

  // 2. Write the TypeDefinition artifact with current_version_id.
  TypeDefinition td;
  td.set_type_name(referencing_type_name);
  td.set_current_version_id(type_version_def_id);
  WriteArtifact(storage, type_def_id, "TypeDefinition", /*version_id=*/1, td.SerializeAsString());

  // 3. Write the TypeVersionDefinition artifact with the descriptor_set.
  TypeVersionDefinition tvd;
  tvd.set_type_id(type_def_id);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(referencing_descriptor);
  WriteArtifact(storage, type_version_def_id, "TypeVersionDefinition", /*version_id=*/1, tvd.SerializeAsString());
}

// Write a ReferenceDefinition artifact to storage.
void WriteReferenceDefinition(MemoryStorage& storage, uint64_t artifact_id, const std::string& key_type, const std::string& target_type_name,
                              const std::string& referencing_type_name, const std::string& field_name, const std::string& covering_index_key_type,
                              ReferenceOption::OnDelete on_delete) {
  ReferenceDefinition ref_def;
  ref_def.set_key_type(key_type);
  ref_def.set_target_type_name(target_type_name);
  ref_def.set_referencing_type_name(referencing_type_name);
  ref_def.set_field_name(field_name);
  ref_def.set_covering_index_key_type(covering_index_key_type);
  ref_def.set_on_delete(on_delete);

  WriteArtifact(storage, artifact_id, "ReferenceDefinition", /*version_id=*/1, ref_def.SerializeAsString());
}

// ===========================================================================
// ValidateReferences tests
// ===========================================================================

TEST(ReferentialIntegrityTest, ValidateReferencesValidTarget) {
  MemoryStorage storage;

  // Write a TypeDefinition artifact at ID 100.
  TypeDefinition td;
  td.set_type_name("SomeType");
  WriteArtifact(storage, /*id=*/100, "TypeDefinition", /*version_id=*/1, td.SerializeAsString());

  // Create a TypeVersionDefinition with type_id = 100.
  TypeVersionDefinition tvd;
  tvd.set_type_id(100);

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->empty());
}

TEST(ReferentialIntegrityTest, ValidateReferencesTargetNotFound) {
  MemoryStorage storage;

  // No artifact exists at ID 999.
  TypeVersionDefinition tvd;
  tvd.set_type_id(999);

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND);
  EXPECT_EQ((*result_or)[0].subject(), "field: type_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesTargetTombstoned) {
  MemoryStorage storage;

  // Write a tombstoned artifact at ID 200.
  WriteTombstone(storage, /*id=*/200, "TypeDefinition", /*version_id=*/1);

  TypeVersionDefinition tvd;
  tvd.set_type_id(200);

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_TOMBSTONED);
  EXPECT_EQ((*result_or)[0].subject(), "field: type_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesTargetWrongType) {
  MemoryStorage storage;

  // Write an artifact at ID 300 but with the wrong type name.
  TypeDefinition td;
  td.set_type_name("WrongType");
  WriteArtifact(storage, /*id=*/300, "NotATypeDefinition", /*version_id=*/1, td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(300);

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_WRONG_TYPE);
  EXPECT_EQ((*result_or)[0].subject(), "field: type_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesImplicitPresenceDefault) {
  // type_id is an implicit-presence uint64 field. A default value of 0 should
  // still be validated (artifact 0 does not exist -> NOT_FOUND).
  MemoryStorage storage;

  TypeVersionDefinition tvd;
  // type_id defaults to 0 (implicit presence, no has_presence).

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND);
  EXPECT_EQ((*result_or)[0].subject(), "field: type_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesFieldWithoutReferenceOptionSkipped) {
  // previous_version_id and next_version_id do NOT have the (references)
  // extension, so they are skipped even if set to a non-existent ID.
  MemoryStorage storage;

  // Write a valid target for type_id so we get no violation from it.
  TypeDefinition td;
  td.set_type_name("SomeType");
  WriteArtifact(storage, /*id=*/100, "TypeDefinition", /*version_id=*/1, td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(100);
  tvd.set_previous_version_id(88888);
  tvd.set_next_version_id(99999);

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeVersionDefinition::descriptor(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->empty());
}

TEST(ReferentialIntegrityTest, ValidateReferencesDescriptorMismatchReturnsError) {
  MemoryStorage storage;

  TypeVersionDefinition tvd;
  // Pass the wrong descriptor.
  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(tvd, *TypeDefinition::descriptor(), ctx);
  ASSERT_FALSE(result_or.ok());
  EXPECT_EQ(result_or.status().code(), absl::StatusCode::kInvalidArgument);
}

// ===========================================================================
// EnforceDeleteIntegrity tests
// ===========================================================================

TEST(ReferentialIntegrityTest, EnforceDeleteNoReferences) {
  // When index_def_ids_by_key_type is empty (no "references_by_target_type"
  // entry), enforcement returns an empty result.
  MemoryStorage storage;
  auto ctx = MakeContext(storage);

  auto result_or = EnforceDeleteIntegrity(/*artifact_id=*/100, "TypeDefinition", ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteNoReferencesForType) {
  // references_by_target_type index exists but has no entries for the type
  // being deleted (index object not found at the path).
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;

  // Write the references_by_target_type index for "OtherType", not
  // "TypeDefinition".
  constexpr uint64_t kRefDefId = 6000;
  WriteReferenceDefinition(storage, kRefDefId, "some_covering", "OtherType", "SomeReferencer", "field", "some_covering", ReferenceOption::RESTRICT);
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "OtherType", {kRefDefId});

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
  };

  // Delete a TypeDefinition -- no index entry for "TypeDefinition" exists.
  auto result_or = EnforceDeleteIntegrity(/*artifact_id=*/100, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteMissingCoveringIndexSkipped) {
  // When the covering index key_type is not in index_def_ids_by_key_type,
  // the reference is silently skipped (no violation, no side effect).
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kRefDefId = 6000;
  const std::string kCoveringKeyType = "type_versions_by_type";

  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "TypeDefinition", "TypeVersionDefinition", "type_id", kCoveringKeyType,
                           ReferenceOption::RESTRICT);

  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});

  auto ctx = MakeContext(storage);
  // Only include the references_by_target_type index; omit the covering index.
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(/*artifact_id=*/100, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteTombstonedRefDefSkipped) {
  // A tombstoned ReferenceDefinition artifact is skipped during enforcement.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kRefDefId = 6000;

  // Write a tombstoned ReferenceDefinition.
  WriteTombstone(storage, kRefDefId, "ReferenceDefinition", /*version_id=*/1);

  // The index still points to the tombstoned artifact.
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(/*artifact_id=*/100, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteRefDefTargetMismatchSkipped) {
  // A ReferenceDefinition whose target_type_name does not match the deleted
  // artifact's type is skipped.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kRefDefId = 6000;
  const std::string kCoveringKeyType = "some_covering";

  // Write a ReferenceDefinition that targets "OtherType", not "TypeDefinition".
  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "OtherType", "SomeReferencer", "some_field", kCoveringKeyType, ReferenceOption::RESTRICT);

  // But the index for "TypeDefinition" (mistakenly) points to it.
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType, 5001},
  };

  auto result_or = EnforceDeleteIntegrity(/*artifact_id=*/100, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteRestrict) {
  // Full RESTRICT enforcement path. The implementation resolves the referencing
  // type's descriptor via: type_name_unique index -> TypeDefinition ->
  // TypeVersionDefinition (descriptor_set) -> message descriptor. This test
  // sets up the full chain and verifies REFERENCE_DELETE_RESTRICTED is returned.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId = 5001;
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kRefDefId = 6000;
  constexpr uint64_t kTargetId = 100;
  constexpr uint64_t kReferencingId = 200;
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType = "type_versions_by_type";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  // Set up the referencing type resolution chain.
  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  // Write the ReferenceDefinition.
  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType,
                           ReferenceOption::RESTRICT);

  // Write the references_by_target_type index.
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});

  // Write the covering index with one referencing artifact.
  WriteCoveringIndex(storage, kCoveringIndexDefId, kTargetId, {kReferencingId}, kCoveringKeyType);

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType, kCoveringIndexDefId},
      {"type_name_unique", kTnuIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(kTargetId, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // RESTRICT: should produce a violation, no side effects.
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED);
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteCascade) {
  // CASCADE enforcement: deleting a TypeDefinition that is referenced by a
  // TypeVersionDefinition should produce a CascadeDelete side effect.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId = 5001;
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kRefDefId = 6000;
  constexpr uint64_t kTargetId = 100;
  constexpr uint64_t kReferencingId = 200;
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType = "type_versions_by_type";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType, ReferenceOption::CASCADE);

  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});
  WriteCoveringIndex(storage, kCoveringIndexDefId, kTargetId, {kReferencingId}, kCoveringKeyType);

  // Write the referencing artifact so CASCADE recursion can read its type_name.
  WriteArtifact(storage, kReferencingId, "TypeVersionDefinition", /*version_id=*/1, "some_payload");

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType, kCoveringIndexDefId},
      {"type_name_unique", kTnuIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(kTargetId, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // CASCADE: no violations, one CascadeDelete side effect.
  EXPECT_TRUE(result_or->violations.empty());
  ASSERT_GE(result_or->side_effects.size(), 1);
  ASSERT_TRUE(std::holds_alternative<CascadeDelete>(result_or->side_effects[0]));
  EXPECT_EQ(std::get<CascadeDelete>(result_or->side_effects[0]).artifact_id, kReferencingId);
}

TEST(ReferentialIntegrityTest, EnforceDeleteSetNull) {
  // SET_NULL enforcement: deleting a TypeDefinition that is referenced by a
  // TypeVersionDefinition should produce a SetNullUpdate side effect.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId = 5001;
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kRefDefId = 6000;
  constexpr uint64_t kTargetId = 100;
  constexpr uint64_t kReferencingId = 200;
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType = "type_versions_by_type";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType,
                           ReferenceOption::SET_NULL);

  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});
  WriteCoveringIndex(storage, kCoveringIndexDefId, kTargetId, {kReferencingId}, kCoveringKeyType);

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType, kCoveringIndexDefId},
      {"type_name_unique", kTnuIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(kTargetId, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // SET_NULL: no violations, one SetNullUpdate side effect.
  EXPECT_TRUE(result_or->violations.empty());
  ASSERT_EQ(result_or->side_effects.size(), 1);
  ASSERT_TRUE(std::holds_alternative<SetNullUpdate>(result_or->side_effects[0]));
  const auto& snu = std::get<SetNullUpdate>(result_or->side_effects[0]);
  EXPECT_EQ(snu.referencing_artifact_id, kReferencingId);
  EXPECT_EQ(snu.field_name, "type_id");
  EXPECT_EQ(snu.removed_reference_id, kTargetId);
}

TEST(ReferentialIntegrityTest, EnforceDeleteScheduledDeletesFiltered) {
  // When the referencing artifact is in scheduled_deletes, it should be
  // filtered out and produce no violations or side effects.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId = 5001;
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kRefDefId = 6000;
  constexpr uint64_t kTargetId = 100;
  constexpr uint64_t kReferencingId = 200;
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType = "type_versions_by_type";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  WriteReferenceDefinition(storage, kRefDefId, kCoveringKeyType, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType,
                           ReferenceOption::RESTRICT);

  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId});
  WriteCoveringIndex(storage, kCoveringIndexDefId, kTargetId, {kReferencingId}, kCoveringKeyType);

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType, kCoveringIndexDefId},
      {"type_name_unique", kTnuIndexDefId},
  };

  // The referencing artifact is already scheduled for deletion.
  std::set<uint64_t> scheduled_deletes = {kReferencingId};

  auto result_or = EnforceDeleteIntegrity(kTargetId, "TypeDefinition", ctx, scheduled_deletes, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->side_effects.empty());
}

TEST(ReferentialIntegrityTest, EnforceDeleteCascadeRecursive) {
  // Test that CASCADE recursively enforces referential integrity on cascaded
  // artifacts. We set up two levels:
  //   TypeDefinition (kTargetId) <- TypeVersionDefinition (kMiddleId) [CASCADE]
  //   TypeVersionDefinition (kMiddleId) <- "ChildArtifact" (kLeafId) [RESTRICT]
  //
  // Deleting the TypeDefinition should cascade to the TypeVersionDefinition,
  // which in turn should find a RESTRICT reference from the ChildArtifact,
  // producing both a CascadeDelete side effect and a RESTRICT violation.
  MemoryStorage storage;

  // Index definition IDs.
  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId1 = 5001; // type_versions_by_type
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kCoveringIndexDefId2 = 5003; // child_by_tvd
  // ReferenceDefinition IDs.
  constexpr uint64_t kRefDefId1 = 6000; // TVD -> TD (CASCADE)
  constexpr uint64_t kRefDefId2 = 6001; // Child -> TVD (RESTRICT)
  // Artifact IDs.
  constexpr uint64_t kTargetId = 100; // TypeDefinition being deleted
  constexpr uint64_t kMiddleId = 200; // TypeVersionDefinition (cascaded)
  constexpr uint64_t kLeafId = 300;   // ChildArtifact (restricts middle)
  // Type resolution IDs.
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType1 = "type_versions_by_type";
  const std::string kCoveringKeyType2 = "child_by_tvd";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  // Set up type resolution for TypeVersionDefinition.
  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  // Level 1: TypeVersionDefinition references TypeDefinition via CASCADE.
  WriteReferenceDefinition(storage, kRefDefId1, kCoveringKeyType1, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType1,
                           ReferenceOption::CASCADE);

  // Level 2: ChildArtifact references TypeVersionDefinition via RESTRICT.
  // We use TypeVersionDefinition as the referencing type for level 2 as well
  // (self-referential for simplicity -- "child_by_tvd" covering index).
  WriteReferenceDefinition(storage, kRefDefId2, kCoveringKeyType2, "TypeVersionDefinition", kReferencingTypeName, "type_id", kCoveringKeyType2,
                           ReferenceOption::RESTRICT);

  // Write references_by_target_type for both target types.
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId1});
  // For the recursive step, "TypeVersionDefinition" is the target type.
  // We need a separate index entry for "TypeVersionDefinition".
  // The implementation uses EncodeStringKey("TypeVersionDefinition") for path.
  // We reuse the same index_def_id since it's the same index type.
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeVersionDefinition", {kRefDefId2});

  // Write covering indexes.
  WriteCoveringIndex(storage, kCoveringIndexDefId1, kTargetId, {kMiddleId}, kCoveringKeyType1);
  WriteCoveringIndex(storage, kCoveringIndexDefId2, kMiddleId, {kLeafId}, kCoveringKeyType2);

  // Write the middle artifact so CASCADE recursion can read its type_name.
  WriteArtifact(storage, kMiddleId, "TypeVersionDefinition", /*version_id=*/1, "middle_payload");

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType1, kCoveringIndexDefId1},
      {kCoveringKeyType2, kCoveringIndexDefId2},
      {"type_name_unique", kTnuIndexDefId},
  };

  auto result_or = EnforceDeleteIntegrity(kTargetId, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // Should have a CascadeDelete for kMiddleId (from level 1 CASCADE).
  bool found_cascade = false;
  for (const auto& effect : result_or->side_effects) {
    if (std::holds_alternative<CascadeDelete>(effect) && std::get<CascadeDelete>(effect).artifact_id == kMiddleId) {
      found_cascade = true;
    }
  }
  EXPECT_TRUE(found_cascade) << "Expected CascadeDelete for middle artifact";

  // Should have a RESTRICT violation from level 2 (kLeafId references kMiddleId).
  ASSERT_GE(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED);
}

TEST(ReferentialIntegrityTest, EnforceDeleteCycleDetection) {
  // Test that mutually-referencing types don't cause infinite recursion.
  // We set up: TypeDefinition (A) <-CASCADE- TypeVersionDefinition (B)
  //            TypeVersionDefinition (B) <-CASCADE- (points back to A)
  // When B is in scheduled_deletes during the recursive call, it should
  // be filtered and not recurse further.
  MemoryStorage storage;

  constexpr uint64_t kRefsIndexDefId = 5000;
  constexpr uint64_t kCoveringIndexDefId1 = 5001;
  constexpr uint64_t kCoveringIndexDefId2 = 5003;
  constexpr uint64_t kTnuIndexDefId = 5002;
  constexpr uint64_t kRefDefId1 = 6000;
  constexpr uint64_t kRefDefId2 = 6001;
  constexpr uint64_t kArtifactA = 100;
  constexpr uint64_t kArtifactB = 200;
  constexpr uint64_t kTvdTypeDefId = 7000;
  constexpr uint64_t kTvdTypeVersionDefId = 7001;
  const std::string kCoveringKeyType1 = "type_versions_by_type";
  const std::string kCoveringKeyType2 = "td_by_tvd";
  const std::string kReferencingTypeName = "artifact_system.TypeVersionDefinition";

  // Set up type resolution for TypeVersionDefinition.
  SetupReferencingTypeResolution(storage, kReferencingTypeName, TypeVersionDefinition::descriptor(), kTnuIndexDefId, kTvdTypeDefId, kTvdTypeVersionDefId);

  // A (TypeDefinition) is referenced by B (TypeVersionDefinition) via CASCADE.
  WriteReferenceDefinition(storage, kRefDefId1, kCoveringKeyType1, "TypeDefinition", kReferencingTypeName, "type_id", kCoveringKeyType1,
                           ReferenceOption::CASCADE);

  // B (TypeVersionDefinition) is referenced by A back via CASCADE.
  // This creates a cycle: deleting A cascades to B, deleting B would try to
  // cascade back to A. scheduled_deletes prevents this.
  WriteReferenceDefinition(storage, kRefDefId2, kCoveringKeyType2, "TypeVersionDefinition", kReferencingTypeName, "type_id", kCoveringKeyType2,
                           ReferenceOption::CASCADE);

  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeDefinition", {kRefDefId1});
  WriteReferencesByTargetTypeIndex(storage, kRefsIndexDefId, "TypeVersionDefinition", {kRefDefId2});

  WriteCoveringIndex(storage, kCoveringIndexDefId1, kArtifactA, {kArtifactB}, kCoveringKeyType1);
  WriteCoveringIndex(storage, kCoveringIndexDefId2, kArtifactB, {kArtifactA}, kCoveringKeyType2);

  // Write both artifacts so CASCADE recursion can read type_name.
  WriteArtifact(storage, kArtifactA, "TypeDefinition", /*version_id=*/1, "payload_a");
  WriteArtifact(storage, kArtifactB, "TypeVersionDefinition", /*version_id=*/1, "payload_b");

  auto ctx = MakeContext(storage);
  std::unordered_map<std::string, uint64_t> index_map = {
      {"references_by_target_type", kRefsIndexDefId},
      {kCoveringKeyType1, kCoveringIndexDefId1},
      {kCoveringKeyType2, kCoveringIndexDefId2},
      {"type_name_unique", kTnuIndexDefId},
  };

  // Should not hang or crash. The implementation filters kArtifactA out of
  // the recursive call's active_referencing because it is the artifact being
  // deleted (artifact_id == ref_artifact_id check).
  auto result_or = EnforceDeleteIntegrity(kArtifactA, "TypeDefinition", ctx, /*scheduled_deletes=*/{}, index_map);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // Should have CascadeDelete for B.
  bool found_cascade_b = false;
  for (const auto& effect : result_or->side_effects) {
    if (std::holds_alternative<CascadeDelete>(effect) && std::get<CascadeDelete>(effect).artifact_id == kArtifactB) {
      found_cascade_b = true;
    }
  }
  EXPECT_TRUE(found_cascade_b) << "Expected CascadeDelete for artifact B";

  // No violations expected -- A is filtered from B's recursive enforcement
  // because the CASCADE code adds cascaded artifacts to updated_scheduled,
  // and the self-reference (artifact_id == ref_artifact_id) is also filtered.
  EXPECT_TRUE(result_or->violations.empty());
}

TEST(ReferentialIntegrityTest, ValidateReferencesRepeatedDuplicateValue) {
  // A repeated uint64 reference field containing duplicate artifact_ids must
  // produce REFERENCE_DUPLICATE_VALUE violations.
  MemoryStorage storage;

  // Write valid target artifacts so the duplicate check is exercised before
  // (or alongside) reference-target validation.
  TypeDefinition td1;
  td1.set_type_name("SomeType");
  WriteArtifact(storage, /*id=*/100, "TypeDefinition", /*version_id=*/1, td1.SerializeAsString());
  TypeDefinition td2;
  td2.set_type_name("AnotherType");
  WriteArtifact(storage, /*id=*/200, "TypeDefinition", /*version_id=*/1, td2.SerializeAsString());

  // Build a dynamic message type with a repeated uint64 field annotated with
  // the (references) extension, using a DescriptorPool backed by the generated
  // pool so artifact_options.proto extensions are available.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto file;
  file.set_name("dup_ref_test.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("DupRefTestArtifact");

  auto* ref_field = message->add_field();
  ref_field->set_name("target_ids");
  ref_field->set_number(1);
  ref_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  ref_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  // Set the (references) extension on the field options.
  auto* ref_option = ref_field->mutable_options()->MutableExtension(artifact_system::references);
  ref_option->set_target_type_name("TypeDefinition");
  ref_option->set_on_delete(artifact_system::ReferenceOption::RESTRICT);

  const auto* built_file = pool.BuildFile(file);
  ASSERT_NE(built_file, nullptr);
  const auto* descriptor = built_file->FindMessageTypeByName("DupRefTestArtifact");
  ASSERT_NE(descriptor, nullptr);

  // Create a DynamicMessage instance and populate the repeated field with
  // duplicate values: [100, 200, 100].
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(descriptor);
  ASSERT_NE(prototype, nullptr);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  const auto* reflection = msg->GetReflection();
  const auto* field = descriptor->FindFieldByName("target_ids");
  ASSERT_NE(field, nullptr);

  reflection->AddUInt64(msg.get(), field, 100);
  reflection->AddUInt64(msg.get(), field, 200);
  reflection->AddUInt64(msg.get(), field, 100); // duplicate

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();

  // Expect exactly one REFERENCE_DUPLICATE_VALUE violation (for the second 100).
  bool found_duplicate = false;
  for (const auto& v : *result_or) {
    if (v.category() == ArtifactWriteViolation::REFERENCE_DUPLICATE_VALUE) {
      found_duplicate = true;
      EXPECT_EQ(v.subject(), "field: target_ids");
    }
  }
  EXPECT_TRUE(found_duplicate) << "Expected REFERENCE_DUPLICATE_VALUE violation";
}

// ===========================================================================
// ValidateReferences nested-message tests
// ===========================================================================

// Build a NestedRefHolder dynamic message from text format.
std::unique_ptr<google::protobuf::Message> MakeNestedRefMessage(const google::protobuf::Descriptor* descriptor,
                                                                google::protobuf::DynamicMessageFactory& factory, const std::string& text_proto) {
  std::unique_ptr<google::protobuf::Message> msg(factory.GetPrototype(descriptor)->New());
  EXPECT_TRUE(google::protobuf::TextFormat::ParseFromString(text_proto, msg.get()));
  return msg;
}

TEST(ReferentialIntegrityTest, ValidateReferencesNestedMessageFieldValid) {
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("SomeType");
  WriteArtifact(storage, /*id=*/100, "TypeDefinition", /*version_id=*/1, td.SerializeAsString());

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, "nested { target_id: 100 }");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->empty());
}

TEST(ReferentialIntegrityTest, ValidateReferencesNestedMessageFieldNotFound) {
  // A dangling reference on a nested message field is reported with the
  // dotted field path.
  MemoryStorage storage;

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, "nested { target_id: 999 }");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND);
  EXPECT_EQ((*result_or)[0].subject(), "field: nested.target_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesRepeatedNestedMessages) {
  // Each element of a repeated nested message field is validated.
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("SomeType");
  WriteArtifact(storage, /*id=*/100, "TypeDefinition", /*version_id=*/1, td.SerializeAsString());

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, "items { target_id: 100 } items { target_id: 999 }");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND);
  EXPECT_EQ((*result_or)[0].subject(), "field: items.target_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesDeeplyNestedField) {
  // Violations two message levels down carry the full dotted path.
  MemoryStorage storage;

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, "outer { inner { target_id: 999 } }");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->size(), 1);
  EXPECT_EQ((*result_or)[0].category(), ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND);
  EXPECT_EQ((*result_or)[0].subject(), "field: outer.inner.target_id");
}

TEST(ReferentialIntegrityTest, ValidateReferencesUnsetNestedMessageSkipped) {
  // Unset singular nested message fields are not descended into. Inner's
  // target_id is implicit-presence, so descending into the default instance
  // would validate 0 and produce a NOT_FOUND violation.
  MemoryStorage storage;

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, "");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->empty());
}

TEST(ReferentialIntegrityTest, ValidateReferencesMapFieldSkipped) {
  // Map fields are not descended into, even when the map value message type
  // carries a reference annotation with a dangling value.
  MemoryStorage storage;

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = CompileProtoSource(kNestedRefProtoSource, "artifact_system.testing.NestedRefHolder", pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto msg = MakeNestedRefMessage(descriptor, factory, R"(entries { key: "a" value { target_id: 999 } })");

  auto ctx = MakeContext(storage);
  auto result_or = ValidateReferences(*msg, *descriptor, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->empty());
}

} // namespace
} // namespace artifact_system::testing

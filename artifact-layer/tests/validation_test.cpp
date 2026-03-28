#include "artifact/validation.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "artifact_options.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "storage/memory_storage.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::artifact::ResolveVersionId;
using artifact_system::artifact::ValidateCreateOrUpdate;
using artifact_system::artifact::ValidateDelete;
using artifact_system::artifact::ValidationContext;
using artifact_system::artifact::ValidationResult;
using artifact_system::artifact::WriteOperation;

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

// Store a StoredArtifact into |storage| on "main" at the given artifact_id.
void PutStoredArtifact(MemoryStorage& storage, uint64_t artifact_id, uint64_t version_id, const std::string& type_name, const std::string& payload) {
  StoredArtifact stored;
  stored.set_version_id(version_id);
  stored.set_type_name(type_name);
  stored.set_payload(payload);
  ASSERT_TRUE(storage.PutObject("main", encoding::ArtifactPath(artifact_id), stored.SerializeAsString()).ok());
}

// Bootstrap a MemoryStorage with:
//   artifact 1: TypeDefinition for "TypeDefinition" (deny_* all false)
//   artifact 2: TypeVersionDefinition (type_id=1, descriptor_set for TD)
void BootstrapStorage(MemoryStorage& storage) {
  // artifact 1 — TypeDefinition for "TypeDefinition"
  TypeDefinition td;
  td.set_type_name("artifact_system.TypeDefinition");
  td.set_deny_create(false);
  td.set_deny_update(false);
  td.set_deny_delete(false);
  PutStoredArtifact(storage, /*artifact_id=*/1, /*version_id=*/2, "TypeDefinition", td.SerializeAsString());

  // artifact 2 — TypeVersionDefinition pointing at type_id=1
  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, /*artifact_id=*/2, /*version_id=*/2, "TypeVersionDefinition", tvd.SerializeAsString());

  ASSERT_TRUE(storage.Commit("main", "bootstrap").ok());
}

// ---------------------------------------------------------------------------
// ResolveVersionId tests
// ---------------------------------------------------------------------------

TEST(ValidationTest, ResolveVersionIdSuccess) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  ValidationContext ctx{&storage, "main"};
  auto resolved_or = ResolveVersionId(/*version_id=*/2, ctx);
  ASSERT_TRUE(resolved_or.ok()) << resolved_or.status();

  const auto& resolved = *resolved_or;
  EXPECT_EQ(resolved.type_def_id, 1);
  EXPECT_EQ(resolved.type_name, "artifact_system.TypeDefinition");
  EXPECT_FALSE(resolved.deny_create);
  EXPECT_FALSE(resolved.deny_update);
  EXPECT_FALSE(resolved.deny_delete);
  EXPECT_EQ(resolved.version_id, 2);
  EXPECT_GT(resolved.descriptor_set.file_size(), 0);
}

TEST(ValidationTest, ResolveVersionIdNotFound) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  ValidationContext ctx{&storage, "main"};
  auto resolved_or = ResolveVersionId(/*version_id=*/999, ctx);
  ASSERT_FALSE(resolved_or.ok());
  EXPECT_EQ(resolved_or.status().code(), absl::StatusCode::kNotFound);
}

TEST(ValidationTest, ResolveVersionIdWrongType) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  // artifact 1 is a TypeDefinition, not a TypeVersionDefinition
  ValidationContext ctx{&storage, "main"};
  auto resolved_or = ResolveVersionId(/*version_id=*/1, ctx);
  ASSERT_FALSE(resolved_or.ok());
  EXPECT_EQ(resolved_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(ValidationTest, ResolveVersionIdTypeDefNotFound) {
  MemoryStorage storage;

  // Write a TVD whose type_id points to a non-existent TypeDefinition.
  TypeVersionDefinition tvd;
  tvd.set_type_id(999);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, /*artifact_id=*/10, /*version_id=*/0, "TypeVersionDefinition", tvd.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap bad tvd").ok());

  ValidationContext ctx{&storage, "main"};
  auto resolved_or = ResolveVersionId(/*version_id=*/10, ctx);
  ASSERT_FALSE(resolved_or.ok());
  EXPECT_EQ(resolved_or.status().code(), absl::StatusCode::kNotFound);
}

// ---------------------------------------------------------------------------
// ValidateCreateOrUpdate tests
// ---------------------------------------------------------------------------

TEST(ValidationTest, ValidateCreateOrUpdateSuccess) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  TypeDefinition payload;
  payload.set_type_name("SomeNewType");

  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, payload.SerializeAsString(), ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  EXPECT_TRUE(result_or->violations.empty());
  EXPECT_TRUE(result_or->resolved_type.has_value());
}

TEST(ValidationTest, ValidateCreateDenyCreate) {
  MemoryStorage storage;

  // TypeDefinition with deny_create=true
  TypeDefinition td;
  td.set_type_name("LockedType");
  td.set_deny_create(true);
  PutStoredArtifact(storage, 1, 2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, 2, 2, "TypeVersionDefinition", tvd.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap deny_create").ok());

  TypeDefinition payload;
  payload.set_type_name("whatever");

  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, 2, payload.SerializeAsString(), ctx);
  ASSERT_TRUE(result_or.ok());
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::MUTATION_DENIED);
}

TEST(ValidationTest, ValidateUpdateDenyUpdate) {
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("LockedType");
  td.set_deny_update(true);
  PutStoredArtifact(storage, 1, 2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, 2, 2, "TypeVersionDefinition", tvd.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap deny_update").ok());

  TypeDefinition payload;
  payload.set_type_name("whatever");

  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kUpdate, 2, payload.SerializeAsString(), ctx);
  ASSERT_TRUE(result_or.ok());
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::MUTATION_DENIED);
}

TEST(ValidationTest, ValidateCreateEmptyPayload) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, /*payload=*/"", ctx);
  ASSERT_TRUE(result_or.ok());
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::EMPTY_PAYLOAD);
}

TEST(ValidationTest, ValidateCreateInvalidPayload) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  ValidationContext ctx{&storage, "main"};
  // Garbage bytes that won't parse as TypeDefinition.
  std::string garbage(64, '\xff');
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, garbage, ctx);
  ASSERT_TRUE(result_or.ok());
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE);
}

TEST(ValidationTest, ValidateCreateWithBypass) {
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("artifact_system.TypeDefinition");
  td.set_deny_create(true);
  PutStoredArtifact(storage, 1, 2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, 2, 2, "TypeVersionDefinition", tvd.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap deny_create bypass").ok());

  TypeDefinition payload;
  payload.set_type_name("whatever");

  ValidationContext ctx{&storage, "main", /*bypass_mutation_check=*/true};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, 2, payload.SerializeAsString(), ctx);
  ASSERT_TRUE(result_or.ok());
  // bypass_mutation_check skips MUTATION_DENIED — should pass.
  EXPECT_TRUE(result_or->violations.empty());
}

TEST(ValidationTest, ValidateUpdateTypeMismatch) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  // Write an existing artifact with a different type_name.
  PutStoredArtifact(storage, /*artifact_id=*/100, /*version_id=*/2, "SomeOtherType", "irrelevant");
  ASSERT_TRUE(storage.Commit("main", "add mismatched artifact").ok());

  TypeDefinition payload;
  payload.set_type_name("whatever");

  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kUpdate, /*version_id=*/2, payload.SerializeAsString(), ctx,
                                          /*existing_artifact_id=*/100);
  ASSERT_TRUE(result_or.ok());
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::INVALID_VERSION_ID);
}

// ---------------------------------------------------------------------------
// ValidateDelete tests
// ---------------------------------------------------------------------------

TEST(ValidationTest, ValidateDeleteDenyDelete) {
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("ProtectedType");
  td.set_deny_delete(true);
  PutStoredArtifact(storage, 1, 2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, 2, 2, "TypeVersionDefinition", tvd.SerializeAsString());

  // The artifact to delete — must reference version_id=2 so ValidateDelete can
  // resolve it.
  TypeDefinition artifact_payload;
  artifact_payload.set_type_name("instance");
  PutStoredArtifact(storage, /*artifact_id=*/50, /*version_id=*/2, "ProtectedType", artifact_payload.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap deny_delete").ok());

  ValidationContext ctx{&storage, "main"};
  auto violations_or = ValidateDelete(/*artifact_id=*/50, ctx);
  ASSERT_TRUE(violations_or.ok());
  ASSERT_EQ(violations_or->size(), 1);
  EXPECT_EQ((*violations_or)[0].category(), ArtifactWriteViolation::MUTATION_DENIED);
}

TEST(ValidationTest, ValidateDeleteSuccess) {
  MemoryStorage storage;
  BootstrapStorage(storage);

  // An artifact of type "TypeDefinition" with deny_delete=false.
  TypeDefinition artifact_payload;
  artifact_payload.set_type_name("deletable");
  PutStoredArtifact(storage, /*artifact_id=*/50, /*version_id=*/2, "TypeDefinition", artifact_payload.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "add deletable artifact").ok());

  ValidationContext ctx{&storage, "main"};
  auto violations_or = ValidateDelete(/*artifact_id=*/50, ctx);
  ASSERT_TRUE(violations_or.ok());
  EXPECT_TRUE(violations_or->empty());
}

// ---------------------------------------------------------------------------
// Short-circuit behavior
// ---------------------------------------------------------------------------

TEST(ValidationTest, ShortCircuitBehavior) {
  MemoryStorage storage;

  // Set up a type with deny_create=true.
  TypeDefinition td;
  td.set_type_name("LockedType");
  td.set_deny_create(true);
  PutStoredArtifact(storage, 1, 2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = BuildDescriptorSet(TypeDefinition::descriptor());
  PutStoredArtifact(storage, 2, 2, "TypeVersionDefinition", tvd.SerializeAsString());
  ASSERT_TRUE(storage.Commit("main", "bootstrap short-circuit").ok());

  ValidationContext ctx{&storage, "main"};

  // INVALID_VERSION_ID prevents MUTATION_DENIED from running.
  {
    auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/999, /*payload=*/"", ctx);
    ASSERT_TRUE(result_or.ok());
    ASSERT_EQ(result_or->violations.size(), 1);
    EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::INVALID_VERSION_ID);
    EXPECT_FALSE(result_or->resolved_type.has_value());
  }

  // MUTATION_DENIED prevents EMPTY_PAYLOAD from running.
  {
    auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, /*payload=*/"", ctx);
    ASSERT_TRUE(result_or.ok());
    ASSERT_EQ(result_or->violations.size(), 1);
    EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::MUTATION_DENIED);
    EXPECT_FALSE(result_or->resolved_type.has_value());
  }

  // With bypass, EMPTY_PAYLOAD is reached.
  {
    ValidationContext bypass_ctx{&storage, "main",
                                 /*bypass_mutation_check=*/true};
    auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, /*payload=*/"", bypass_ctx);
    ASSERT_TRUE(result_or.ok());
    ASSERT_EQ(result_or->violations.size(), 1);
    EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::EMPTY_PAYLOAD);
    EXPECT_FALSE(result_or->resolved_type.has_value());
  }

  // PAYLOAD_VALIDATION_FAILURE prevents index derivation from running.
  {
    ValidationContext bypass_ctx{&storage, "main", /*bypass_mutation_check=*/true};
    std::string garbage(64, '\xff');
    auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, garbage, bypass_ctx);
    ASSERT_TRUE(result_or.ok());
    ASSERT_EQ(result_or->violations.size(), 1);
    EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE);
    EXPECT_FALSE(result_or->resolved_type.has_value());
  }
}

TEST(ValidationTest, ValidateCreateNanInIndexedField) {
  // Build a custom type with a float indexed field, store it in a
  // MemoryStorage as TypeDefinition + TypeVersionDefinition, then call
  // ValidateCreateOrUpdate with a payload containing NaN in that field.
  // This exercises Phase 5 (NAN_IN_INDEXED_FIELD) of validation.

  // Step 1: Build a descriptor with a float indexed field using a
  // DescriptorPool backed by the generated pool (for artifact_options.proto).
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto file;
  file.set_name("nan_test.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("NanTestArtifact");

  auto* score_field = message->add_field();
  score_field->set_name("score");
  score_field->set_number(1);
  score_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  score_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_FLOAT);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_score");
  index->add_key("score");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const auto* built_file = pool.BuildFile(file);
  ASSERT_NE(built_file, nullptr);
  const auto* descriptor = built_file->FindMessageTypeByName("NanTestArtifact");
  ASSERT_NE(descriptor, nullptr);

  // Step 2: Build the FileDescriptorSet from the pool descriptor.
  google::protobuf::FileDescriptorSet custom_fds;
  {
    std::set<const google::protobuf::FileDescriptor*> seen;
    std::function<void(const google::protobuf::FileDescriptor*)> add_file;
    add_file = [&](const google::protobuf::FileDescriptor* fd) {
      if (!seen.insert(fd).second)
        return;
      for (int i = 0; i < fd->dependency_count(); ++i) {
        add_file(fd->dependency(i));
      }
      fd->CopyTo(custom_fds.add_file());
    };
    add_file(descriptor->file());
  }

  // Step 3: Build a payload with NaN in the score field.
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(descriptor);
  ASSERT_NE(prototype, nullptr);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  const auto* reflection = msg->GetReflection();
  reflection->SetFloat(msg.get(), descriptor->FindFieldByName("score"), std::nanf(""));

  std::string payload;
  ASSERT_TRUE(msg->SerializeToString(&payload));

  // Step 4: Bootstrap storage with a TypeDefinition and TypeVersionDefinition
  // that use the custom type.
  MemoryStorage storage;

  TypeDefinition td;
  td.set_type_name("artifact_system.testing.NanTestArtifact");
  td.set_deny_create(false);
  td.set_deny_update(false);
  td.set_deny_delete(false);
  PutStoredArtifact(storage, /*artifact_id=*/1, /*version_id=*/2, "TypeDefinition", td.SerializeAsString());

  TypeVersionDefinition tvd;
  tvd.set_type_id(1);
  *tvd.mutable_descriptor_set() = custom_fds;
  PutStoredArtifact(storage, /*artifact_id=*/2, /*version_id=*/2, "TypeVersionDefinition", tvd.SerializeAsString());

  ASSERT_TRUE(storage.Commit("main", "bootstrap nan test").ok());

  // Step 5: Validate — should produce NAN_IN_INDEXED_FIELD.
  ValidationContext ctx{&storage, "main"};
  auto result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, /*version_id=*/2, payload, ctx);
  ASSERT_TRUE(result_or.ok()) << result_or.status();
  ASSERT_EQ(result_or->violations.size(), 1);
  EXPECT_EQ(result_or->violations[0].category(), ArtifactWriteViolation::NAN_IN_INDEXED_FIELD);
  EXPECT_TRUE(result_or->resolved_type.has_value());
}

} // namespace
} // namespace artifact_system::testing

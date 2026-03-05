#include <gtest/gtest.h>

// Generated proto headers.
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_service.grpc.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

namespace artifact_system {
namespace {

// ---------------------------------------------------------------------------
// artifact_options.proto — custom option extensions
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, OrderDefinitionFields) {
  OrderDefinition od;
  od.set_field("artifact_id");
  od.set_direction(OrderDefinition::ASCENDING);
  EXPECT_EQ(od.field(), "artifact_id");
  EXPECT_EQ(od.direction(), OrderDefinition::ASCENDING);

  // Verify all enum values exist.
  EXPECT_EQ(OrderDefinition::ORDER_BY_UNSPECIFIED, 0);
  EXPECT_EQ(OrderDefinition::ASCENDING, 1);
  EXPECT_EQ(OrderDefinition::DESCENDING, 2);
}

TEST(ProtoCompilationTest, IndexDefinitionFields) {
  IndexDefinition id;
  id.set_key_type("by_owner");
  id.add_key("created_by");
  id.set_unique(true);

  auto* order = id.add_order();
  order->set_field("artifact_id");
  order->set_direction(OrderDefinition::ASCENDING);

  EXPECT_EQ(id.key_type(), "by_owner");
  EXPECT_EQ(id.key_size(), 1);
  EXPECT_EQ(id.key(0), "created_by");
  EXPECT_TRUE(id.unique());
  EXPECT_EQ(id.order_size(), 1);
}

TEST(ProtoCompilationTest, ReferenceOptionFields) {
  ReferenceOption ro;
  ro.set_target_type_name("TypeDefinition");
  ro.set_on_delete(ReferenceOption::RESTRICT);
  EXPECT_EQ(ro.target_type_name(), "TypeDefinition");
  EXPECT_EQ(ro.on_delete(), ReferenceOption::RESTRICT);

  // Verify all OnDelete enum values exist.
  EXPECT_EQ(ReferenceOption::ON_DELETE_UNSPECIFIED, 0);
  EXPECT_EQ(ReferenceOption::RESTRICT, 1);
  EXPECT_EQ(ReferenceOption::CASCADE, 2);
  EXPECT_EQ(ReferenceOption::SET_NULL, 3);
}

TEST(ProtoCompilationTest, CustomOptionsAccessible) {
  // Verify that the custom options extensions are registered and accessible
  // via reflection.
  const auto* pool = google::protobuf::DescriptorPool::generated_pool();

  // Message-level: indexes extension on MessageOptions.
  const auto* msg_opts_desc = pool->FindMessageTypeByName("google.protobuf.MessageOptions");
  ASSERT_NE(msg_opts_desc, nullptr);
  const auto* indexes_ext = pool->FindExtensionByNumber(msg_opts_desc, 50002);
  ASSERT_NE(indexes_ext, nullptr);
  EXPECT_EQ(indexes_ext->name(), "indexes");
  EXPECT_TRUE(indexes_ext->is_repeated());

  // Message-level: message_description extension on MessageOptions.
  const auto* msg_desc_ext = pool->FindExtensionByNumber(msg_opts_desc, 50004);
  ASSERT_NE(msg_desc_ext, nullptr);
  EXPECT_EQ(msg_desc_ext->name(), "message_description");

  // Field-level: references extension on FieldOptions.
  const auto* field_opts_desc = pool->FindMessageTypeByName("google.protobuf.FieldOptions");
  ASSERT_NE(field_opts_desc, nullptr);
  const auto* refs_ext = pool->FindExtensionByNumber(field_opts_desc, 50003);
  ASSERT_NE(refs_ext, nullptr);
  EXPECT_EQ(refs_ext->name(), "references");

  // Field-level: field_description extension on FieldOptions.
  const auto* field_desc_ext = pool->FindExtensionByNumber(field_opts_desc, 50005);
  ASSERT_NE(field_desc_ext, nullptr);
  EXPECT_EQ(field_desc_ext->name(), "field_description");
}

// ---------------------------------------------------------------------------
// artifact_types.proto — built-in type messages
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, TypeDefinitionFields) {
  TypeDefinition td;
  td.set_type_name("MyType");
  td.set_current_version_id(42);
  td.set_deny_create(true);
  td.set_deny_update(false);
  td.set_deny_delete(true);

  EXPECT_EQ(td.type_name(), "MyType");
  EXPECT_TRUE(td.has_current_version_id());
  EXPECT_EQ(td.current_version_id(), 42);
  EXPECT_TRUE(td.deny_create());
  EXPECT_FALSE(td.deny_update());
  EXPECT_TRUE(td.deny_delete());
}

TEST(ProtoCompilationTest, TypeVersionDefinitionFields) {
  TypeVersionDefinition tvd;
  tvd.set_type_id(100);
  tvd.set_proto_source("syntax = \"proto3\";");
  tvd.set_previous_version_id(99);
  // next_version_id left unset (optional).

  EXPECT_EQ(tvd.type_id(), 100);
  EXPECT_TRUE(tvd.has_previous_version_id());
  EXPECT_EQ(tvd.previous_version_id(), 99);
  EXPECT_FALSE(tvd.has_next_version_id());

  // descriptor_set is a nested FileDescriptorSet.
  auto* fds = tvd.mutable_descriptor_set();
  fds->add_file()->set_name("test.proto");
  EXPECT_EQ(tvd.descriptor_set().file_size(), 1);
}

TEST(ProtoCompilationTest, ReferenceDefinitionFields) {
  ReferenceDefinition rd;
  rd.set_key_type("DataFrameArtifact.created_by");
  rd.set_target_type_name("User");
  rd.set_referencing_type_name("DataFrameArtifact");
  rd.set_field_name("created_by");
  rd.set_covering_index_key_type("by_owner");
  rd.set_on_delete(ReferenceOption::RESTRICT);

  EXPECT_EQ(rd.key_type(), "DataFrameArtifact.created_by");
  EXPECT_EQ(rd.target_type_name(), "User");
  EXPECT_EQ(rd.on_delete(), ReferenceOption::RESTRICT);
}

// ---------------------------------------------------------------------------
// artifact_service.proto — services and error messages
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, ReadContextOneof) {
  ReadContext rc;
  EXPECT_EQ(rc.context_case(), ReadContext::CONTEXT_NOT_SET);

  rc.set_snapshot_id(1);
  EXPECT_EQ(rc.context_case(), ReadContext::kSnapshotId);

  rc.set_transaction_id(2);
  EXPECT_EQ(rc.context_case(), ReadContext::kTransactionId);
}

TEST(ProtoCompilationTest, SnapshotTransactionErrorCategories) {
  EXPECT_EQ(SnapshotTransactionError::CATEGORY_UNSPECIFIED, 0);
  EXPECT_EQ(SnapshotTransactionError::PARENT_NOT_FOUND, 1);
  EXPECT_EQ(SnapshotTransactionError::TRANSACTION_NOT_FOUND, 2);
  EXPECT_EQ(SnapshotTransactionError::SNAPSHOT_NOT_FOUND, 3);
}

TEST(ProtoCompilationTest, ArtifactWriteViolationCategories) {
  // Verify all 11 violation categories exist (plus UNSPECIFIED).
  EXPECT_EQ(ArtifactWriteViolation::CATEGORY_UNSPECIFIED, 0);
  EXPECT_EQ(ArtifactWriteViolation::INVALID_VERSION_ID, 1);
  EXPECT_EQ(ArtifactWriteViolation::MUTATION_DENIED, 2);
  EXPECT_EQ(ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE, 3);
  EXPECT_EQ(ArtifactWriteViolation::EMPTY_PAYLOAD, 4);
  EXPECT_EQ(ArtifactWriteViolation::NAN_IN_INDEXED_FIELD, 5);
  EXPECT_EQ(ArtifactWriteViolation::NON_MINIMAL_VARINT, 6);
  EXPECT_EQ(ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND, 7);
  EXPECT_EQ(ArtifactWriteViolation::REFERENCE_TARGET_TOMBSTONED, 8);
  EXPECT_EQ(ArtifactWriteViolation::REFERENCE_TARGET_WRONG_TYPE, 9);
  EXPECT_EQ(ArtifactWriteViolation::REFERENCE_DUPLICATE_VALUE, 10);
  EXPECT_EQ(ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED, 11);
}

TEST(ProtoCompilationTest, CommitConflictMessage) {
  CommitConflict cc;
  cc.set_conflict_type(CommitConflict::INDEX_CONFLICT);
  cc.set_retryable(true);
  cc.set_attempts(3);

  auto* idx = cc.mutable_index_detail();
  idx->set_key_type("by_owner");
  idx->set_encoded_key("key_bytes");

  EXPECT_EQ(cc.conflict_type(), CommitConflict::INDEX_CONFLICT);
  EXPECT_TRUE(cc.retryable());
  EXPECT_EQ(cc.attempts(), 3);
  EXPECT_EQ(cc.detail_case(), CommitConflict::kIndexDetail);

  // Verify all conflict types.
  EXPECT_EQ(CommitConflict::CONFLICT_TYPE_UNSPECIFIED, 0);
  EXPECT_EQ(CommitConflict::INDEX_CONFLICT, 1);
  EXPECT_EQ(CommitConflict::PAYLOAD_CONFLICT, 2);
  EXPECT_EQ(CommitConflict::REFERENTIAL_INTEGRITY_VIOLATION, 3);
}

TEST(ProtoCompilationTest, CommitConflictPayloadDetail) {
  CommitConflict cc;
  auto* pd = cc.mutable_payload_detail();
  pd->set_artifact_id(42);
  EXPECT_EQ(cc.detail_case(), CommitConflict::kPayloadDetail);
  EXPECT_EQ(cc.payload_detail().artifact_id(), 42);
}

TEST(ProtoCompilationTest, CommitConflictReferentialIntegrityDetail) {
  CommitConflict cc;
  auto* rid = cc.mutable_referential_integrity_detail();
  rid->set_target_artifact_id(100);
  rid->set_reference_key_type("DataFrameArtifact.created_by");
  rid->add_referencing_artifact_ids(200);
  rid->add_referencing_artifact_ids(201);
  EXPECT_EQ(cc.detail_case(), CommitConflict::kReferentialIntegrityDetail);
  EXPECT_EQ(rid->referencing_artifact_ids_size(), 2);
}

TEST(ProtoCompilationTest, CommitConflictOptionalCommitIds) {
  CommitConflict cc;
  EXPECT_FALSE(cc.has_base_commit_id());
  EXPECT_FALSE(cc.has_ours_commit_id());
  EXPECT_FALSE(cc.has_theirs_commit_id());

  cc.set_base_commit_id("abc");
  cc.set_ours_commit_id("def");
  cc.set_theirs_commit_id("ghi");
  EXPECT_TRUE(cc.has_base_commit_id());
  EXPECT_EQ(cc.base_commit_id(), "abc");
}

TEST(ProtoCompilationTest, FetchIndexErrorCategories) {
  EXPECT_EQ(FetchIndexError::CATEGORY_UNSPECIFIED, 0);
  EXPECT_EQ(FetchIndexError::INDEX_NOT_FOUND, 1);
  EXPECT_EQ(FetchIndexError::INCOMPLETE_KEY, 2);
  EXPECT_EQ(FetchIndexError::KEY_PARSE_FAILURE, 3);
}

TEST(ProtoCompilationTest, TypeRegistrationViolationCategories) {
  EXPECT_EQ(TypeRegistrationViolation::CATEGORY_UNSPECIFIED, 0);
  EXPECT_EQ(TypeRegistrationViolation::PROTO_COMPILATION_FAILURE, 1);
  EXPECT_EQ(TypeRegistrationViolation::SCHEMA_INCOMPATIBILITY, 2);
  EXPECT_EQ(TypeRegistrationViolation::INVALID_INDEX_DEFINITION, 3);
  EXPECT_EQ(TypeRegistrationViolation::INDEX_INCOMPATIBILITY, 4);
  EXPECT_EQ(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION, 5);
  EXPECT_EQ(TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY, 6);
  EXPECT_EQ(TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION, 7);
}

TEST(ProtoCompilationTest, CrudRequestResponseRoundTrip) {
  // CreateArtifact
  CreateArtifactRequest create_req;
  create_req.set_version_id(1);
  create_req.set_payload("binary_data");
  create_req.set_transaction_id(100);
  EXPECT_TRUE(create_req.has_transaction_id());

  CreateArtifactResponse create_resp;
  create_resp.set_artifact_id(42);
  create_resp.set_snapshot_id(200);

  // GetArtifact
  GetArtifactRequest get_req;
  get_req.set_artifact_id(42);
  get_req.mutable_context()->set_snapshot_id(200);

  GetArtifactResponse get_resp;
  get_resp.set_artifact_id(42);
  get_resp.set_type_name("MyType");
  get_resp.set_version_id(1);
  get_resp.set_payload("binary_data");

  // BatchGetArtifacts
  BatchGetArtifactsRequest batch_req;
  batch_req.add_artifact_ids(42);
  batch_req.add_artifact_ids(43);

  BatchGetArtifactsResponse batch_resp;
  batch_resp.add_results()->mutable_artifact()->set_artifact_id(42);
  batch_resp.add_results()->mutable_not_found()->set_artifact_id(43);
  EXPECT_EQ(batch_resp.results(0).result_case(), ArtifactResult::kArtifact);
  EXPECT_EQ(batch_resp.results(1).result_case(), ArtifactResult::kNotFound);

  // UpdateArtifact
  UpdateArtifactRequest update_req;
  update_req.set_artifact_id(42);
  update_req.set_version_id(2);
  update_req.set_payload("new_data");

  // DeleteArtifact
  DeleteArtifactRequest delete_req;
  delete_req.set_artifact_id(42);
  EXPECT_FALSE(delete_req.has_transaction_id());
}

TEST(ProtoCompilationTest, ArtifactNotFoundError) {
  ArtifactNotFoundError err;
  err.set_artifact_id(42);
  err.set_tombstoned(true);
  EXPECT_EQ(err.artifact_id(), 42);
  EXPECT_TRUE(err.tombstoned());
}

TEST(ProtoCompilationTest, SnapshotTransactionMessages) {
  CreateSnapshotRequest snap_req;
  EXPECT_FALSE(snap_req.has_parent_id());
  snap_req.set_parent_id(10);
  EXPECT_TRUE(snap_req.has_parent_id());

  CreateTransactionRequest tx_req;
  EXPECT_FALSE(tx_req.has_parent_id());

  CommitTransactionRequest commit_req;
  commit_req.set_transaction_id(10);
  EXPECT_EQ(commit_req.transaction_id(), 10);

  CommitTransactionResponse commit_resp;
  commit_resp.set_snapshot_id(200);
  EXPECT_EQ(commit_resp.snapshot_id(), 200);

  RollbackTransactionRequest rollback_req;
  rollback_req.set_transaction_id(10);

  // RollbackTransactionResponse is empty.
  RollbackTransactionResponse rollback_resp;
  EXPECT_EQ(rollback_resp.ByteSizeLong(), 0);
}

TEST(ProtoCompilationTest, FetchIndexMessages) {
  FetchIndexRequest req;
  req.set_key_type("by_owner");
  req.set_key("key_bytes");
  req.mutable_context()->set_transaction_id(5);

  FetchIndexResponse resp;
  resp.set_index_payload("index_data");
  resp.set_index_message_name("Index_MyType_by_owner");
}

TEST(ProtoCompilationTest, RegisterTypeVersionMessages) {
  RegisterTypeVersionRequest req;
  req.set_type_name("MyType");
  req.set_proto_source("syntax = \"proto3\";\nmessage MyType {}");
  req.set_deny_create(true);
  EXPECT_TRUE(req.has_deny_create());
  EXPECT_FALSE(req.has_deny_update());
  EXPECT_FALSE(req.has_deny_delete());

  RegisterTypeVersionResponse resp;
  resp.set_version_id(1);
}

TEST(ProtoCompilationTest, GetTypeVersionMessages) {
  GetTypeVersionRequest req;
  req.set_version_id(1);

  GetTypeVersionResponse resp;
  resp.set_version_id(1);
  resp.set_type_id(100);
  resp.mutable_descriptor_set()->add_file()->set_name("test.proto");
  resp.set_proto_source("syntax = \"proto3\";");
  resp.set_previous_version_id(0);
  EXPECT_TRUE(resp.has_previous_version_id());
  EXPECT_FALSE(resp.has_next_version_id());
}

TEST(ProtoCompilationTest, ListTypeVersionsMessages) {
  ListTypeVersionsRequest req;
  req.set_type_name("MyType");

  ListTypeVersionsResponse resp;
  resp.add_version_ids(1);
  resp.add_version_ids(2);
  EXPECT_EQ(resp.version_ids_size(), 2);
}

TEST(ProtoCompilationTest, GetIndexSchemaMessages) {
  GetIndexSchemaRequest req;
  req.set_key_type("by_owner");

  GetIndexSchemaResponse resp;
  resp.set_index_definition_id(10);
  resp.set_key_type("by_owner");
  resp.add_key_fields("created_by");
  resp.set_unique(false);
  resp.set_key_message_name("IndexKey_MyType_by_owner");
  resp.set_value_message_name("IndexValue_MyType_by_owner");
  resp.set_index_message_name("Index_MyType_by_owner");

  auto* order = resp.add_order_fields();
  order->set_field("artifact_id");
  order->set_direction(OrderDefinition::ASCENDING);

  resp.mutable_index_descriptor_set()->add_file()->set_name("generated.proto");
}

// ---------------------------------------------------------------------------
// artifact_internal.proto — StoredArtifact envelope
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, StoredArtifactFields) {
  StoredArtifact sa;
  sa.set_envelope_version(1);
  sa.set_version_id(42);
  sa.set_type_name("MyType");
  sa.set_payload("binary_data");

  EXPECT_EQ(sa.envelope_version(), 1);
  EXPECT_EQ(sa.version_id(), 42);
  EXPECT_EQ(sa.type_name(), "MyType");
  EXPECT_EQ(sa.payload(), "binary_data");

  // Tombstone = empty payload.
  StoredArtifact tombstone;
  tombstone.set_envelope_version(1);
  tombstone.set_version_id(42);
  tombstone.set_type_name("MyType");
  // payload left empty.
  EXPECT_TRUE(tombstone.payload().empty());
}

// ---------------------------------------------------------------------------
// gRPC service descriptors (verify all 4 services are generated)
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, GrpcServicesExist) {
  const auto* pool = google::protobuf::DescriptorPool::generated_pool();

  const auto* snap_tx_service = pool->FindServiceByName("artifact_system.SnapshotTransactionService");
  ASSERT_NE(snap_tx_service, nullptr);
  EXPECT_EQ(snap_tx_service->method_count(), 4);

  const auto* artifact_service = pool->FindServiceByName("artifact_system.ArtifactService");
  ASSERT_NE(artifact_service, nullptr);
  EXPECT_EQ(artifact_service->method_count(), 5);

  const auto* index_service = pool->FindServiceByName("artifact_system.IndexService");
  ASSERT_NE(index_service, nullptr);
  EXPECT_EQ(index_service->method_count(), 1);

  const auto* registry_service = pool->FindServiceByName("artifact_system.TypeRegistryService");
  ASSERT_NE(registry_service, nullptr);
  EXPECT_EQ(registry_service->method_count(), 4);
}

// ---------------------------------------------------------------------------
// Index options on built-in types (verify self-referential annotations)
// ---------------------------------------------------------------------------

TEST(ProtoCompilationTest, IndexDefinitionHasSelfReferentialIndexes) {
  // IndexDefinition in artifact_options.proto should have two index annotations:
  // index_key_type_unique and all_index_definitions.
  const auto* desc = IndexDefinition::descriptor();
  ASSERT_NE(desc, nullptr);
  const auto& opts = desc->options();

  int index_count = opts.ExtensionSize(indexes);
  EXPECT_EQ(index_count, 2);

  // First: index_key_type_unique.
  const auto& idx0 = opts.GetExtension(indexes, 0);
  EXPECT_EQ(idx0.key_type(), "index_key_type_unique");
  EXPECT_EQ(idx0.key_size(), 1);
  EXPECT_EQ(idx0.key(0), "key_type");
  EXPECT_TRUE(idx0.unique());

  // Second: all_index_definitions.
  const auto& idx1 = opts.GetExtension(indexes, 1);
  EXPECT_EQ(idx1.key_type(), "all_index_definitions");
  EXPECT_EQ(idx1.key_size(), 0);
  EXPECT_FALSE(idx1.unique());
}

TEST(ProtoCompilationTest, TypeDefinitionHasIndexOptions) {
  const auto* desc = TypeDefinition::descriptor();
  ASSERT_NE(desc, nullptr);
  const auto& opts = desc->options();

  int index_count = opts.ExtensionSize(indexes);
  EXPECT_EQ(index_count, 2);

  const auto& idx0 = opts.GetExtension(indexes, 0);
  EXPECT_EQ(idx0.key_type(), "type_name_unique");
  EXPECT_TRUE(idx0.unique());

  const auto& idx1 = opts.GetExtension(indexes, 1);
  EXPECT_EQ(idx1.key_type(), "all_types");
  EXPECT_FALSE(idx1.unique());
}

TEST(ProtoCompilationTest, TypeVersionDefinitionHasIndexAndReferenceOptions) {
  const auto* desc = TypeVersionDefinition::descriptor();
  ASSERT_NE(desc, nullptr);

  // Index option.
  const auto& msg_opts = desc->options();
  int index_count = msg_opts.ExtensionSize(indexes);
  EXPECT_EQ(index_count, 1);
  EXPECT_EQ(msg_opts.GetExtension(indexes, 0).key_type(), "type_versions_by_type");

  // Reference option on type_id field.
  const auto* type_id_field = desc->FindFieldByName("type_id");
  ASSERT_NE(type_id_field, nullptr);
  const auto& field_opts = type_id_field->options();
  EXPECT_TRUE(field_opts.HasExtension(references));
  const auto& ref = field_opts.GetExtension(references);
  EXPECT_EQ(ref.target_type_name(), "TypeDefinition");
  EXPECT_EQ(ref.on_delete(), ReferenceOption::RESTRICT);
}

TEST(ProtoCompilationTest, ReferenceDefinitionHasIndexOptions) {
  const auto* desc = ReferenceDefinition::descriptor();
  ASSERT_NE(desc, nullptr);
  const auto& opts = desc->options();

  int index_count = opts.ExtensionSize(indexes);
  EXPECT_EQ(index_count, 3);

  EXPECT_EQ(opts.GetExtension(indexes, 0).key_type(), "reference_key_type_unique");
  EXPECT_TRUE(opts.GetExtension(indexes, 0).unique());

  EXPECT_EQ(opts.GetExtension(indexes, 1).key_type(), "references_by_target_type");
  EXPECT_FALSE(opts.GetExtension(indexes, 1).unique());

  EXPECT_EQ(opts.GetExtension(indexes, 2).key_type(), "all_reference_definitions");
  EXPECT_FALSE(opts.GetExtension(indexes, 2).unique());
}

} // namespace
} // namespace artifact_system

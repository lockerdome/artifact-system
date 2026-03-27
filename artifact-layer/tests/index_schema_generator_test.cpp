#include "index/index_schema_generator.h"

#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "google/protobuf/descriptor.pb.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

TEST(IndexSchemaGeneratorTest, GeneratesExpectedMessagesForBuiltinTypeIndex) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const auto& options = descriptor->options();
  ASSERT_GT(options.ExtensionSize(artifact_system::indexes), 0);
  const artifact_system::IndexDefinition& definition = options.GetExtension(artifact_system::indexes, 0);
  auto schema_or = index::GenerateIndexSchema(definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;

  ASSERT_NE(schema.key_descriptor, nullptr);
  ASSERT_NE(schema.value_descriptor, nullptr);
  ASSERT_NE(schema.index_descriptor, nullptr);

  const auto* key_field = schema.key_descriptor->FindFieldByNumber(1);
  ASSERT_NE(key_field, nullptr);
  EXPECT_EQ(key_field->type(), google::protobuf::FieldDescriptor::TYPE_STRING);
  EXPECT_TRUE(key_field->has_presence());

  const auto* row_count = schema.value_descriptor->FindFieldByName("row_count");
  ASSERT_NE(row_count, nullptr);
  EXPECT_EQ(row_count->type(), google::protobuf::FieldDescriptor::TYPE_UINT32);

  const auto* order_field = schema.value_descriptor->FindFieldByNumber(2);
  ASSERT_NE(order_field, nullptr);
  EXPECT_EQ(order_field->name(), "artifact_id");
  EXPECT_TRUE(order_field->is_repeated());
  EXPECT_EQ(order_field->type(), google::protobuf::FieldDescriptor::TYPE_UINT64);

  const auto* key_message_field = schema.index_descriptor->FindFieldByName("key");
  const auto* value_message_field = schema.index_descriptor->FindFieldByName("value");
  ASSERT_NE(key_message_field, nullptr);
  ASSERT_NE(value_message_field, nullptr);
  EXPECT_EQ(key_message_field->message_type(), schema.key_descriptor);
  EXPECT_EQ(value_message_field->message_type(), schema.value_descriptor);
}

TEST(IndexSchemaGeneratorTest, SupportsEnumsDefinedInImportedFiles) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto enum_file;
  enum_file.set_name("index_schema_generator_enum.proto");
  enum_file.set_package("artifact_system.testing");
  enum_file.set_syntax("proto3");
  auto* status_enum = enum_file.add_enum_type();
  status_enum->set_name("Status");
  auto* status_unknown = status_enum->add_value();
  status_unknown->set_name("STATUS_UNKNOWN");
  status_unknown->set_number(0);
  auto* status_ready = status_enum->add_value();
  status_ready->set_name("STATUS_READY");
  status_ready->set_number(1);
  ASSERT_NE(pool.BuildFile(enum_file), nullptr);

  google::protobuf::FileDescriptorProto message_file;
  message_file.set_name("index_schema_generator_message.proto");
  message_file.set_package("artifact_system.testing");
  message_file.set_syntax("proto3");
  message_file.add_dependency("artifact_options.proto");
  message_file.add_dependency("index_schema_generator_enum.proto");

  auto* message = message_file.add_message_type();
  message->set_name("EnumIndexedArtifact");
  auto* status_field = message->add_field();
  status_field->set_name("status");
  status_field->set_number(1);
  status_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  status_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_ENUM);
  status_field->set_type_name("Status");

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_status");
  index->add_key("status");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built_file = pool.BuildFile(message_file);
  ASSERT_NE(built_file, nullptr);
  const auto* descriptor = built_file->FindMessageTypeByName("EnumIndexedArtifact");
  ASSERT_NE(descriptor, nullptr);

  const auto& options = descriptor->options();
  ASSERT_EQ(options.ExtensionSize(artifact_system::indexes), 1);
  const artifact_system::IndexDefinition& definition = options.GetExtension(artifact_system::indexes, 0);

  auto schema_or = index::GenerateIndexSchema(definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;
  const auto* key_field = schema.key_descriptor->FindFieldByNumber(1);
  ASSERT_NE(key_field, nullptr);
  EXPECT_EQ(key_field->type(), google::protobuf::FieldDescriptor::TYPE_ENUM);
  ASSERT_NE(key_field->enum_type(), nullptr);
  EXPECT_EQ(key_field->enum_type()->full_name(), "artifact_system.testing.Status");
}

TEST(IndexSchemaGeneratorTest, RejectsOrderWithoutArtifactId) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const auto& options = descriptor->options();
  ASSERT_GT(options.ExtensionSize(artifact_system::indexes), 0);
  artifact_system::IndexDefinition definition = options.GetExtension(artifact_system::indexes, 0);

  definition.clear_order();
  auto* order = definition.add_order();
  order->set_field("type_name");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  auto schema_or = index::GenerateIndexSchema(definition, *descriptor);
  ASSERT_FALSE(schema_or.ok());
  EXPECT_EQ(schema_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexSchemaGeneratorTest, RejectsDuplicateArtifactIdOrderFields) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const auto& options = descriptor->options();
  ASSERT_GT(options.ExtensionSize(artifact_system::indexes), 0);
  artifact_system::IndexDefinition definition = options.GetExtension(artifact_system::indexes, 0);

  auto* duplicate = definition.add_order();
  duplicate->set_field("artifact_id");
  duplicate->set_direction(artifact_system::OrderDefinition::ASCENDING);

  auto schema_or = index::GenerateIndexSchema(definition, *descriptor);
  ASSERT_FALSE(schema_or.ok());
  EXPECT_EQ(schema_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexSchemaGeneratorTest, RejectsOrderByUnspecifiedDirection) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const auto& options = descriptor->options();
  ASSERT_GT(options.ExtensionSize(artifact_system::indexes), 0);
  artifact_system::IndexDefinition definition = options.GetExtension(artifact_system::indexes, 0);

  definition.clear_order();
  auto* order = definition.add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED);

  auto schema_or = index::GenerateIndexSchema(definition, *descriptor);
  ASSERT_FALSE(schema_or.ok());
  EXPECT_EQ(schema_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexSchemaGeneratorTest, GeneratesMultiKeyAndMultiOrderSchema) {
  // Build a descriptor with two key fields and two order fields to exercise
  // multi-column schema generation and correct field numbering.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto file;
  file.set_name("index_schema_generator_multi.proto");
  file.set_package("artifact_system.testing");
  file.set_syntax("proto3");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("MultiColumnArtifact");

  auto* repo = message->add_field();
  repo->set_name("repo_id");
  repo->set_number(1);
  repo->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  repo->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  auto* owner = message->add_field();
  owner->set_name("owner");
  owner->set_number(2);
  owner->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  owner->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* created_at = message->add_field();
  created_at->set_name("created_at");
  created_at->set_number(3);
  created_at->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  created_at->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT64);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_repo_owner");
  index->add_key("repo_id");
  index->add_key("owner");
  auto* order_created = index->add_order();
  order_created->set_field("created_at");
  order_created->set_direction(artifact_system::OrderDefinition::DESCENDING);
  auto* order_id = index->add_order();
  order_id->set_field("artifact_id");
  order_id->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built_file = pool.BuildFile(file);
  ASSERT_NE(built_file, nullptr);
  const auto* descriptor = built_file->FindMessageTypeByName("MultiColumnArtifact");
  ASSERT_NE(descriptor, nullptr);

  const auto& opts = descriptor->options();
  ASSERT_EQ(opts.ExtensionSize(artifact_system::indexes), 1);
  const artifact_system::IndexDefinition& def = opts.GetExtension(artifact_system::indexes, 0);

  auto schema_or = index::GenerateIndexSchema(def, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;

  // IndexKey has 2 key fields.
  EXPECT_EQ(schema.key_descriptor->field_count(), 2);
  const auto* key1 = schema.key_descriptor->FindFieldByNumber(1);
  const auto* key2 = schema.key_descriptor->FindFieldByNumber(2);
  ASSERT_NE(key1, nullptr);
  ASSERT_NE(key2, nullptr);
  EXPECT_EQ(key1->type(), google::protobuf::FieldDescriptor::TYPE_UINT64);
  EXPECT_EQ(key2->type(), google::protobuf::FieldDescriptor::TYPE_STRING);

  // IndexValue has row_count + 2 order columns (created_at, artifact_id).
  EXPECT_EQ(schema.value_descriptor->field_count(), 3);
  const auto* row_count = schema.value_descriptor->FindFieldByName("row_count");
  ASSERT_NE(row_count, nullptr);
  EXPECT_EQ(row_count->type(), google::protobuf::FieldDescriptor::TYPE_UINT32);

  const auto* created_col = schema.value_descriptor->FindFieldByNumber(2);
  ASSERT_NE(created_col, nullptr);
  EXPECT_EQ(created_col->type(), google::protobuf::FieldDescriptor::TYPE_INT64);
  EXPECT_TRUE(created_col->is_repeated());

  const auto* artifact_id_col = schema.value_descriptor->FindFieldByNumber(3);
  ASSERT_NE(artifact_id_col, nullptr);
  EXPECT_EQ(artifact_id_col->type(), google::protobuf::FieldDescriptor::TYPE_UINT64);
  EXPECT_TRUE(artifact_id_col->is_repeated());

  // value_fields vector matches order columns.
  ASSERT_EQ(schema.value_fields.size(), 2U);
  EXPECT_EQ(schema.value_fields[0], created_col);
  EXPECT_EQ(schema.value_fields[1], artifact_id_col);
}

} // namespace
} // namespace artifact_system::testing

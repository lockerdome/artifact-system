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

} // namespace
} // namespace artifact_system::testing

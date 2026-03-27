#include "index/index_object.h"

#include <cstdint>

#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "index/index_schema_generator.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

artifact_system::IndexDefinition FindIndexDefinitionByKeyType(const google::protobuf::Descriptor& descriptor, const std::string& key_type) {
  const auto& options = descriptor.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    const artifact_system::IndexDefinition& index = options.GetExtension(artifact_system::indexes, i);
    if (index.key_type() == key_type) {
      return index;
    }
  }
  return {};
}

TEST(IndexObjectTest, RoundTripsThroughDeterministicSerialization) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* key_prototype = factory.GetPrototype(schema.key_descriptor);
  ASSERT_NE(key_prototype, nullptr);
  std::unique_ptr<google::protobuf::Message> key_message(key_prototype->New());
  const auto* key_field = schema.key_descriptor->FindFieldByNumber(1);
  key_message->GetReflection()->SetString(key_message.get(), key_field, "TypeDefinition");
  std::string serialized_key;
  ASSERT_TRUE(key_message->SerializeToString(&serialized_key));

  index::IndexObject object;
  object.serialized_key = serialized_key;
  object.rows = {
      index::IndexRow{.artifact_id = 11, .order_values = {static_cast<uint64_t>(11)}},
      index::IndexRow{.artifact_id = 22, .order_values = {static_cast<uint64_t>(22)}},
  };

  auto bytes_or = index::SerializeIndexObject(schema, index_definition, object);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();

  auto parsed_or = index::DeserializeIndexObject(schema, index_definition, *bytes_or);
  ASSERT_TRUE(parsed_or.ok()) << parsed_or.status();

  ASSERT_EQ(parsed_or->rows.size(), 2U);
  EXPECT_EQ(parsed_or->rows[0].artifact_id, 11U);
  EXPECT_EQ(std::get<uint64_t>(parsed_or->rows[0].order_values[0]), 11U);
  EXPECT_EQ(parsed_or->rows[1].artifact_id, 22U);
  EXPECT_EQ(std::get<uint64_t>(parsed_or->rows[1].order_values[0]), 22U);
}

TEST(IndexObjectTest, RejectsRowCountMismatch) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* index_prototype = factory.GetPrototype(schema.index_descriptor);
  ASSERT_NE(index_prototype, nullptr);
  std::unique_ptr<google::protobuf::Message> index_message(index_prototype->New());
  const auto* index_reflection = index_message->GetReflection();

  const auto* value_field = schema.index_descriptor->FindFieldByName("value");
  google::protobuf::Message* value_message = index_reflection->MutableMessage(index_message.get(), value_field);
  const auto* value_reflection = value_message->GetReflection();
  const auto* row_count_field = schema.value_descriptor->FindFieldByName("row_count");
  value_reflection->SetUInt32(value_message, row_count_field, 2);

  const auto* artifact_id_column = schema.value_descriptor->FindFieldByNumber(2);
  value_reflection->AddUInt64(value_message, artifact_id_column, 1);

  std::string bytes;
  ASSERT_TRUE(index_message->SerializeToString(&bytes));

  auto parsed_or = index::DeserializeIndexObject(schema, index_definition, bytes);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexObjectTest, AllowsTombstonedIndexWithZeroRows) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();

  index::IndexObject object;
  object.serialized_key = std::string{"\n\x0eTypeDefinition", 16};

  auto bytes_or = index::SerializeIndexObject(*schema_or, index_definition, object);
  ASSERT_TRUE(bytes_or.ok()) << bytes_or.status();

  auto parsed_or = index::DeserializeIndexObject(*schema_or, index_definition, *bytes_or);
  ASSERT_TRUE(parsed_or.ok()) << parsed_or.status();
  EXPECT_TRUE(parsed_or->rows.empty());
}

TEST(IndexObjectTest, RejectsMissingKeyForNonEmptyKeyIndex) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();

  index::IndexObject object;
  object.rows = {index::IndexRow{.artifact_id = 11, .order_values = {static_cast<uint64_t>(11)}}};

  auto bytes_or = index::SerializeIndexObject(*schema_or, index_definition, object);
  ASSERT_FALSE(bytes_or.ok());
  EXPECT_EQ(bytes_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexObjectTest, RejectsArtifactIdMismatchWithOrderColumn) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();

  index::IndexObject object;
  object.serialized_key = std::string{"\n\x0eTypeDefinition", 16};
  object.rows = {index::IndexRow{.artifact_id = 22, .order_values = {static_cast<uint64_t>(11)}}};

  auto bytes_or = index::SerializeIndexObject(*schema_or, index_definition, object);
  ASSERT_FALSE(bytes_or.ok());
  EXPECT_EQ(bytes_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexObjectTest, RejectsDuplicateArtifactIdsOnSerialize) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();

  index::IndexObject object;
  object.serialized_key = std::string{"\n\x0eTypeDefinition", 16};
  object.rows = {
      index::IndexRow{.artifact_id = 11, .order_values = {static_cast<uint64_t>(11)}},
      index::IndexRow{.artifact_id = 11, .order_values = {static_cast<uint64_t>(11)}},
  };

  auto bytes_or = index::SerializeIndexObject(*schema_or, index_definition, object);
  ASSERT_FALSE(bytes_or.ok());
  EXPECT_EQ(bytes_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexObjectTest, RejectsDuplicateArtifactIdsOnDeserialize) {
  const auto* descriptor = artifact_system::TypeDefinition::descriptor();
  ASSERT_NE(descriptor, nullptr);

  const artifact_system::IndexDefinition index_definition = FindIndexDefinitionByKeyType(*descriptor, "type_name_unique");
  ASSERT_FALSE(index_definition.key_type().empty());

  auto schema_or = index::GenerateIndexSchema(index_definition, *descriptor);
  ASSERT_TRUE(schema_or.ok()) << schema_or.status();
  const index::GeneratedIndexSchema& schema = *schema_or;

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* index_prototype = factory.GetPrototype(schema.index_descriptor);
  ASSERT_NE(index_prototype, nullptr);
  std::unique_ptr<google::protobuf::Message> index_message(index_prototype->New());
  const auto* index_reflection = index_message->GetReflection();

  const auto* value_field = schema.index_descriptor->FindFieldByName("value");
  google::protobuf::Message* value_message = index_reflection->MutableMessage(index_message.get(), value_field);
  const auto* value_reflection = value_message->GetReflection();
  const auto* row_count_field = schema.value_descriptor->FindFieldByName("row_count");
  value_reflection->SetUInt32(value_message, row_count_field, 2);

  const auto* artifact_id_column = schema.value_descriptor->FindFieldByNumber(2);
  value_reflection->AddUInt64(value_message, artifact_id_column, 11);
  value_reflection->AddUInt64(value_message, artifact_id_column, 11);

  std::string bytes;
  ASSERT_TRUE(index_message->SerializeToString(&bytes));

  auto parsed_or = index::DeserializeIndexObject(schema, index_definition, bytes);
  ASSERT_FALSE(parsed_or.ok());
  EXPECT_EQ(parsed_or.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace artifact_system::testing

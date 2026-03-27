#include "index/index_derivation.h"

#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

#include "absl/status/status.h"
#include "artifact_options.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

const google::protobuf::Descriptor* BuildDescriptorWithIndexes(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_test.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* nested = file.add_message_type();
  nested->set_name("MaybeFields");
  auto* nested_rank = nested->add_field();
  nested_rank->set_name("rank");
  nested_rank->set_number(1);
  nested_rank->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  nested_rank->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);
  auto* nested_score = nested->add_field();
  nested_score->set_name("score");
  nested_score->set_number(2);
  nested_score->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  nested_score->set_type(google::protobuf::FieldDescriptorProto::TYPE_FLOAT);

  auto* message = file.add_message_type();
  message->set_name("IndexedArtifact");

  auto* repo = message->add_field();
  repo->set_name("repo");
  repo->set_number(1);
  repo->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  repo->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* tags = message->add_field();
  tags->set_name("tags");
  tags->set_number(2);
  tags->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  tags->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* maybe = message->add_field();
  maybe->set_name("maybe");
  maybe->set_number(3);
  maybe->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  maybe->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  maybe->set_type_name("MaybeFields");

  auto add_index = [message](const std::string& key_type, const std::string& key_field) {
    auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
    index->set_key_type(key_type);
    index->add_key(key_field);
    auto* order = index->add_order();
    order->set_field("artifact_id");
    order->set_direction(artifact_system::OrderDefinition::ASCENDING);
  };

  add_index("by_repo", "repo");
  add_index("by_tag", "tags");
  add_index("by_rank", "maybe.rank");
  add_index("by_score", "maybe.score");

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("IndexedArtifact");
}

std::unique_ptr<google::protobuf::Message> NewMessage(const google::protobuf::Descriptor* descriptor, google::protobuf::DynamicMessageFactory* factory) {
  const auto* prototype = factory->GetPrototype(descriptor);
  if (prototype == nullptr) {
    return nullptr;
  }
  return std::unique_ptr<google::protobuf::Message>(prototype->New());
}

const google::protobuf::Descriptor* BuildDescriptorWithInvalidIntermediatePath(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_invalid_intermediate.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("BadIntermediateArtifact");

  auto* repo = message->add_field();
  repo->set_name("repo");
  repo->set_number(1);
  repo->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  repo->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("bad_key");
  index->add_key("repo.segment");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("BadIntermediateArtifact");
}

const google::protobuf::Descriptor* BuildDescriptorWithRepeatedMessagePath(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_repeated_message.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* child = file.add_message_type();
  child->set_name("Child");
  auto* child_id = child->add_field();
  child_id->set_name("id");
  child_id->set_number(1);
  child_id->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  child_id->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  auto* message = file.add_message_type();
  message->set_name("BadRepeatedArtifact");

  auto* items = message->add_field();
  items->set_name("items");
  items->set_number(1);
  items->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  items->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  items->set_type_name("Child");

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("bad_repeated");
  index->add_key("items.id");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("BadRepeatedArtifact");
}

const google::protobuf::Descriptor* BuildDescriptorAllScalarTypes(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_all_scalars.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* enum_type = file.add_enum_type();
  enum_type->set_name("Mode");
  auto* enum_a = enum_type->add_value();
  enum_a->set_name("MODE_A");
  enum_a->set_number(0);
  auto* enum_b = enum_type->add_value();
  enum_b->set_name("MODE_B");
  enum_b->set_number(2);

  auto* message = file.add_message_type();
  message->set_name("AllScalars");

  auto add_field = [message](const std::string& name, int number, google::protobuf::FieldDescriptorProto::Type type, const std::string& type_name = "") {
    auto* field = message->add_field();
    field->set_name(name);
    field->set_number(number);
    field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
    field->set_type(type);
    if (!type_name.empty()) {
      field->set_type_name(type_name);
    }
  };

  add_field("f_int32", 1, google::protobuf::FieldDescriptorProto::TYPE_INT32);
  add_field("f_uint32", 2, google::protobuf::FieldDescriptorProto::TYPE_UINT32);
  add_field("f_int64", 3, google::protobuf::FieldDescriptorProto::TYPE_INT64);
  add_field("f_uint64", 4, google::protobuf::FieldDescriptorProto::TYPE_UINT64);
  add_field("f_bool", 5, google::protobuf::FieldDescriptorProto::TYPE_BOOL);
  add_field("f_enum", 6, google::protobuf::FieldDescriptorProto::TYPE_ENUM, "Mode");
  add_field("f_double", 7, google::protobuf::FieldDescriptorProto::TYPE_DOUBLE);
  add_field("f_string", 8, google::protobuf::FieldDescriptorProto::TYPE_STRING);
  add_field("f_bytes", 9, google::protobuf::FieldDescriptorProto::TYPE_BYTES);

  const std::vector<std::string> key_fields = {"f_int32", "f_uint32", "f_int64", "f_uint64", "f_bool", "f_enum", "f_double", "f_string", "f_bytes"};
  for (const std::string& key_field : key_fields) {
    auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
    index->set_key_type("by_" + key_field);
    index->add_key(key_field);
    auto* order = index->add_order();
    order->set_field("artifact_id");
    order->set_direction(artifact_system::OrderDefinition::ASCENDING);
  }

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("AllScalars");
}

const google::protobuf::Descriptor* BuildDescriptorWithPackedRepeatedVarintField(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_packed_varint.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("PackedArtifact");

  auto* ids = message->add_field();
  ids->set_name("ids");
  ids->set_number(1);
  ids->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  ids->set_type(google::protobuf::FieldDescriptorProto::TYPE_INT32);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_id");
  index->add_key("ids");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("PackedArtifact");
}

const google::protobuf::Descriptor* BuildDescriptorWithTwoRepeatedKeyFields(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_two_repeated.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("TwoRepeatedArtifact");

  auto* tags = message->add_field();
  tags->set_name("tags");
  tags->set_number(1);
  tags->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  tags->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* owners = message->add_field();
  owners->set_name("owners");
  owners->set_number(2);
  owners->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  owners->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_tag_owner");
  index->add_key("tags");
  index->add_key("owners");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("TwoRepeatedArtifact");
}

const google::protobuf::Descriptor* BuildDescriptorWithDependency(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto dep_file;
  dep_file.set_name("index_derivation_dep.proto");
  dep_file.set_syntax("proto3");
  dep_file.set_package("artifact_system.testing");

  auto* dep_message = dep_file.add_message_type();
  dep_message->set_name("DepMessage");
  auto* dep_repo = dep_message->add_field();
  dep_repo->set_name("repo");
  dep_repo->set_number(1);
  dep_repo->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  dep_repo->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  if (pool->BuildFile(dep_file) == nullptr) {
    return nullptr;
  }

  google::protobuf::FileDescriptorProto root_file;
  root_file.set_name("index_derivation_dep_root.proto");
  root_file.set_syntax("proto3");
  root_file.set_package("artifact_system.testing");
  root_file.add_dependency("artifact_options.proto");
  root_file.add_dependency("index_derivation_dep.proto");

  auto* root_message = root_file.add_message_type();
  root_message->set_name("DependentArtifact");
  auto* dep_field = root_message->add_field();
  dep_field->set_name("dep");
  dep_field->set_number(1);
  dep_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  dep_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  dep_field->set_type_name("DepMessage");

  auto* index = root_message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_dep_repo");
  index->add_key("dep.repo");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const google::protobuf::FileDescriptor* built = pool->BuildFile(root_file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("DependentArtifact");
}

} // namespace

TEST(IndexDerivationTest, DerivesSimpleRepeatedOptionalAndMultipleIndexes) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "one");
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "two");
  google::protobuf::Message* maybe = reflection->MutableMessage(message.get(), descriptor->FindFieldByName("maybe"));
  const auto* maybe_desc = maybe->GetDescriptor();
  const auto* maybe_reflection = maybe->GetReflection();
  maybe_reflection->SetInt32(maybe, maybe_desc->FindFieldByName("rank"), 7);
  maybe_reflection->SetFloat(maybe, maybe_desc->FindFieldByName("score"), 1.25F);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  // by_repo (1) + by_tag (2 repeated entries) + by_rank (1) + by_score (1)
  ASSERT_EQ(derived_or->size(), 5U);
}

TEST(IndexDerivationTest, PopulatesIndexDefinitionIdsWhenProvided) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");

  const std::unordered_map<std::string, uint64_t> ids = {
      {"by_repo", 111},
      {"by_tag", 222},
      {"by_rank", 333},
      {"by_score", 444},
  };

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42, ids);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 1U);
  EXPECT_EQ(derived_or->at(0).key_type, "by_repo");
  EXPECT_EQ(derived_or->at(0).index_def_id, 111U);
}

TEST(IndexDerivationTest, MissingOptionalKeySkipsIndexEntry) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "one");
  // 'maybe' is not set, so maybe.rank and maybe.score indexes are skipped.

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 2U);
}

TEST(IndexDerivationTest, RejectsNaNInIndexedField) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");
  google::protobuf::Message* maybe = reflection->MutableMessage(message.get(), descriptor->FindFieldByName("maybe"));
  const auto* maybe_desc = maybe->GetDescriptor();
  maybe->GetReflection()->SetFloat(maybe, maybe_desc->FindFieldByName("score"), std::nanf(""));

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 99);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexDerivationTest, RejectsNonMessageIntermediatePath) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithInvalidIntermediatePath(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 7);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexDerivationTest, RejectsRepeatedMessageIntermediatePath) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithRepeatedMessagePath(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 7);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexDerivationTest, DerivesEntriesForAllScalarKeyTypes) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorAllScalarTypes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetInt32(message.get(), descriptor->FindFieldByName("f_int32"), -4);
  reflection->SetUInt32(message.get(), descriptor->FindFieldByName("f_uint32"), 7U);
  reflection->SetInt64(message.get(), descriptor->FindFieldByName("f_int64"), -8);
  reflection->SetUInt64(message.get(), descriptor->FindFieldByName("f_uint64"), 11U);
  reflection->SetBool(message.get(), descriptor->FindFieldByName("f_bool"), true);
  reflection->SetEnumValue(message.get(), descriptor->FindFieldByName("f_enum"), 2);
  reflection->SetDouble(message.get(), descriptor->FindFieldByName("f_double"), 3.25);
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_string"), "hello");
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_bytes"), std::string("\x01\x02", 2));

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 123);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  EXPECT_EQ(derived_or->size(), 9U);
}

TEST(IndexDerivationTest, RejectsUnknownNonMinimalVarintTagInPayload) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");

  google::protobuf::FileDescriptorSet descriptor_set;
  descriptor->file()->CopyTo(descriptor_set.add_file());

  // Field 1 tag with non-minimal varint encoding (0x88 0x00 instead of 0x08), then value 1.
  const std::string bad_payload("\x88\x00\x01", 3);
  auto derived_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), bad_payload, 1, {});
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(std::string(derived_or.status().message()), "NON_MINIMAL_VARINT");
}

TEST(IndexDerivationTest, RejectsNonMinimalVarintsInPackedRepeatedFields) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithPackedRepeatedVarintField(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::FileDescriptorSet descriptor_set;
  descriptor->file()->CopyTo(descriptor_set.add_file());

  // packed ids field (tag=1, wire=2), len=2, value=1 encoded non-minimally (0x81 0x00).
  const std::string bad_payload("\x0A\x02\x81\x00", 4);
  auto derived_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), bad_payload, 1, {});
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_EQ(std::string(derived_or.status().message()), "NON_MINIMAL_VARINT");
}

TEST(IndexDerivationTest, RejectsIndexesWithMoreThanOneRepeatedField) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithTwoRepeatedKeyFields(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "a");
  reflection->AddString(message.get(), descriptor->FindFieldByName("owners"), "b");

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 7);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(derived_or.status().message()).find("INVALID_INDEX_DEFINITION"), std::string::npos);
}

TEST(IndexDerivationTest, DeriveFromPayloadSupportsUnorderedDescriptorSet) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithDependency(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  const auto* reflection = message->GetReflection();
  google::protobuf::Message* dep = reflection->MutableMessage(message.get(), descriptor->FindFieldByName("dep"));
  dep->GetReflection()->SetString(dep, dep->GetDescriptor()->FindFieldByName("repo"), "alpha");

  std::string payload;
  ASSERT_TRUE(message->SerializeToString(&payload));

  google::protobuf::FileDescriptorSet descriptor_set;
  const auto* root_file = descriptor->file();
  root_file->CopyTo(descriptor_set.add_file());
  root_file->dependency(1)->CopyTo(descriptor_set.add_file());

  auto derived_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), payload, 42, {{"by_dep_repo", 1}});
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 1U);
  EXPECT_EQ(derived_or->at(0).index_def_id, 1U);
}

} // namespace artifact_system::testing

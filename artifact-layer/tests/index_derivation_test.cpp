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

TEST(IndexDerivationTest, RepeatedMessageIntermediatePathProducesEntries) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithRepeatedMessagePath(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  const auto* items_fd = descriptor->FindFieldByName("items");
  ASSERT_NE(items_fd, nullptr);

  // Add two Child items with ids 100 and 200.
  auto* item0 = reflection->AddMessage(message.get(), items_fd);
  item0->GetReflection()->SetUInt64(item0, item0->GetDescriptor()->FindFieldByName("id"), 100);
  auto* item1 = reflection->AddMessage(message.get(), items_fd);
  item1->GetReflection()->SetUInt64(item1, item1->GetDescriptor()->FindFieldByName("id"), 200);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 7);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 2U);
  EXPECT_EQ(derived_or->at(0).key_type, "bad_repeated");
  EXPECT_EQ(derived_or->at(1).key_type, "bad_repeated");
  // Verify key values correspond to the two item ids.
  ASSERT_EQ(derived_or->at(0).key_values.size(), 1U);
  ASSERT_EQ(derived_or->at(1).key_values.size(), 1U);
  EXPECT_EQ(std::get<uint64_t>(derived_or->at(0).key_values[0]), 100U);
  EXPECT_EQ(std::get<uint64_t>(derived_or->at(1).key_values[0]), 200U);
}

TEST(IndexDerivationTest, RepeatedMessageIntermediateEmptyProducesNoEntries) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithRepeatedMessagePath(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  // No items added -- empty repeated field.
  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 7);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  EXPECT_EQ(derived_or->size(), 0U);
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

TEST(IndexDerivationTest, DeduplicatesRepeatedFieldValues) {
  // PRD: repeated scalar fields produce "one entry per element value (distinct
  // values only)". Duplicate values in the repeated field must be collapsed.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithIndexes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  const auto* reflection = message->GetReflection();

  reflection->SetString(message.get(), descriptor->FindFieldByName("repo"), "alpha");
  // Add duplicate tag values.
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "dup");
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "dup");
  reflection->AddString(message.get(), descriptor->FindFieldByName("tags"), "other");

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 1);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  // Count entries for the "by_tag" index -- should be 2 (dup, other), not 3.
  int tag_entries = 0;
  for (const auto& entry : *derived_or) {
    if (entry.key_type == "by_tag") {
      ++tag_entries;
    }
  }
  EXPECT_EQ(tag_entries, 2);
}

TEST(IndexDerivationTest, RejectsNaNInIndexedDoubleField) {
  // The existing RejectsNaNInIndexedField test covers float. This covers
  // the double path (TYPE_DOUBLE).
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorAllScalarTypes(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* reflection = message->GetReflection();
  reflection->SetInt32(message.get(), descriptor->FindFieldByName("f_int32"), 1);
  reflection->SetUInt32(message.get(), descriptor->FindFieldByName("f_uint32"), 1);
  reflection->SetInt64(message.get(), descriptor->FindFieldByName("f_int64"), 1);
  reflection->SetUInt64(message.get(), descriptor->FindFieldByName("f_uint64"), 1);
  reflection->SetBool(message.get(), descriptor->FindFieldByName("f_bool"), false);
  reflection->SetEnumValue(message.get(), descriptor->FindFieldByName("f_enum"), 0);
  reflection->SetDouble(message.get(), descriptor->FindFieldByName("f_double"), std::nan(""));
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_string"), "x");
  reflection->SetString(message.get(), descriptor->FindFieldByName("f_bytes"), "y");

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 1);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(derived_or.status().message()).find("NAN_IN_INDEXED_FIELD"), std::string::npos);
}

TEST(IndexDerivationTest, AcceptsPayloadWithNonMinimalVarintOnNonIndexedField) {
  // Build a descriptor with one indexed string field ("name", field 1) and one
  // non-indexed uint32 field ("count", field 2). A payload where field 2's tag
  // uses a non-minimal varint encoding is valid protobuf and must be accepted.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto file;
  file.set_name("non_minimal_wire_test.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("NonMinimalWireArtifact");

  auto* name_field = message->add_field();
  name_field->set_name("name");
  name_field->set_number(1);
  name_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* count_field = message->add_field();
  count_field->set_name("count");
  count_field->set_number(2);
  count_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  count_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT32);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_name");
  index->add_key("name");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const auto* built = pool.BuildFile(file);
  ASSERT_NE(built, nullptr);
  const auto* descriptor = built->FindMessageTypeByName("NonMinimalWireArtifact");
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::FileDescriptorSet descriptor_set;
  descriptor->file()->CopyTo(descriptor_set.add_file());

  // Hand-craft a payload:
  //   field 1 (name), wire type 2: tag=0x0A, len=2, "hi"
  //   field 2 (count), wire type 0: tag encoded non-minimally as 0x90 0x00
  //     (field_number=2, wire_type=0 -> tag value=16=0x10, but encoded as 2-byte varint)
  //     value = 0x05
  // Protobuf parsers accept non-minimal varints; this payload is valid.
  const std::string payload("\x0A\x02hi\x90\x00\x05", 7);
  auto derived_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), payload, 42, {{"by_name", 100}});
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 1U);
  EXPECT_EQ(derived_or->at(0).key_type, "by_name");
  EXPECT_EQ(derived_or->at(0).index_def_id, 100U);
}

TEST(IndexDerivationTest, AcceptsPayloadWithNonMinimalVarintOnIndexedFieldProducesCorrectKey) {
  // A string field that IS an index key is encoded with a non-minimal length
  // prefix in the protobuf wire format. The protobuf library parses the logical
  // value correctly, and our key codec re-encodes the length prefix minimally.
  // The derivation must succeed and produce deterministic encoded_key bytes.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());

  google::protobuf::FileDescriptorProto file;
  file.set_name("non_minimal_indexed_wire_test.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* message = file.add_message_type();
  message->set_name("NonMinimalIndexedWireArtifact");

  auto* name_field = message->add_field();
  name_field->set_name("name");
  name_field->set_number(1);
  name_field->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  name_field->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_name");
  index->add_key("name");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const auto* built = pool.BuildFile(file);
  ASSERT_NE(built, nullptr);
  const auto* descriptor = built->FindMessageTypeByName("NonMinimalIndexedWireArtifact");
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::FileDescriptorSet descriptor_set;
  descriptor->file()->CopyTo(descriptor_set.add_file());

  // Hand-craft a payload where the string field's wire-format length prefix is
  // non-minimal: field 1 tag = 0x0A (minimal), length = 2 encoded as 0x82 0x00
  // (non-minimal 2-byte varint for value 2), then "hi".
  const std::string non_minimal_payload("\x0A\x82\x00hi", 5);

  // Also build the canonical (minimal-varint) payload for comparison.
  const std::string canonical_payload("\x0A\x02hi", 4);

  auto non_minimal_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), non_minimal_payload, 42, {});
  ASSERT_TRUE(non_minimal_or.ok()) << non_minimal_or.status();
  ASSERT_EQ(non_minimal_or->size(), 1U);

  auto canonical_or = index::DeriveIndexEntriesFromPayload(descriptor_set, std::string(descriptor->full_name()), canonical_payload, 42, {});
  ASSERT_TRUE(canonical_or.ok()) << canonical_or.status();
  ASSERT_EQ(canonical_or->size(), 1U);

  // Both payloads encode the same logical value ("hi"), so the key codec must
  // produce identical encoded_key bytes (minimal varint length prefix + "hi").
  EXPECT_EQ(non_minimal_or->at(0).encoded_key, canonical_or->at(0).encoded_key);

  // Verify the encoded key uses minimal varint: length=2 -> 0x02, then "hi".
  const std::vector<uint8_t> expected_key = {0x02, 'h', 'i'};
  EXPECT_EQ(canonical_or->at(0).encoded_key, expected_key);
}

// ---------------------------------------------------------------------------
// Nested repeated field tests
// ---------------------------------------------------------------------------

const google::protobuf::Descriptor* BuildDescriptorWithNestedRepeated(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_nested_repeated.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  // message Input { string name = 1; bool required = 2; repeated uint64 types = 3; }
  auto* input_msg = file.add_message_type();
  input_msg->set_name("Input");
  auto* input_name = input_msg->add_field();
  input_name->set_name("name");
  input_name->set_number(1);
  input_name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  input_name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);
  auto* input_required = input_msg->add_field();
  input_required->set_name("required");
  input_required->set_number(2);
  input_required->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  input_required->set_type(google::protobuf::FieldDescriptorProto::TYPE_BOOL);
  auto* input_types = input_msg->add_field();
  input_types->set_name("types");
  input_types->set_number(3);
  input_types->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  input_types->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  // message NestedRepeatedArtifact { repeated Input inputs = 1; bool is_view_capability = 2; }
  auto* message = file.add_message_type();
  message->set_name("NestedRepeatedArtifact");

  auto* inputs = message->add_field();
  inputs->set_name("inputs");
  inputs->set_number(1);
  inputs->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  inputs->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  inputs->set_type_name("Input");

  auto* is_view = message->add_field();
  is_view->set_name("is_view_capability");
  is_view->set_number(2);
  is_view->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  is_view->set_type(google::protobuf::FieldDescriptorProto::TYPE_BOOL);

  // Index: key=["inputs.types", "is_view_capability"],
  //        order=[inputs._index ASC, inputs.name ASC, inputs.required ASC, artifact_id ASC]
  {
    auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
    index->set_key_type("by_input_type");
    index->add_key("inputs.types");
    index->add_key("is_view_capability");
    auto* o1 = index->add_order();
    o1->set_field("inputs._index");
    o1->set_direction(artifact_system::OrderDefinition::ASCENDING);
    auto* o2 = index->add_order();
    o2->set_field("inputs.name");
    o2->set_direction(artifact_system::OrderDefinition::ASCENDING);
    auto* o3 = index->add_order();
    o3->set_field("inputs.required");
    o3->set_direction(artifact_system::OrderDefinition::ASCENDING);
    auto* o4 = index->add_order();
    o4->set_field("artifact_id");
    o4->set_direction(artifact_system::OrderDefinition::ASCENDING);
  }

  // Index: key=["inputs.types"], order=[artifact_id ASC]
  {
    auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
    index->set_key_type("by_input_type_ref");
    index->add_key("inputs.types");
    auto* o = index->add_order();
    o->set_field("artifact_id");
    o->set_direction(artifact_system::OrderDefinition::ASCENDING);
  }

  const google::protobuf::FileDescriptor* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("NestedRepeatedArtifact");
}

// Helper: add an Input element to a NestedRepeatedArtifact message.
void AddInput(google::protobuf::Message* message, const google::protobuf::Descriptor* desc, const std::string& name, bool required,
              const std::vector<uint64_t>& types) {
  const auto* inputs_fd = desc->FindFieldByName("inputs");
  auto* input = message->GetReflection()->AddMessage(message, inputs_fd);
  const auto* input_desc = input->GetDescriptor();
  const auto* input_refl = input->GetReflection();
  input_refl->SetString(input, input_desc->FindFieldByName("name"), name);
  input_refl->SetBool(input, input_desc->FindFieldByName("required"), required);
  for (uint64_t t : types) {
    input_refl->AddUInt64(input, input_desc->FindFieldByName("types"), t);
  }
}

TEST(IndexDerivationTest, NestedRepeatedProducesCorrelatedRows) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), true);
  AddInput(message.get(), descriptor, "in1", true, {100, 200});
  AddInput(message.get(), descriptor, "in2", false, {300});

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  // Filter to "by_input_type" index.
  std::vector<const index::DerivedIndexEntry*> entries;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type") {
      entries.push_back(&e);
    }
  }
  // 3 rows: inputs[0].types[0]=100, inputs[0].types[1]=200, inputs[1].types[0]=300
  ASSERT_EQ(entries.size(), 3U);

  // Entry 0: key=(100, true), order=(0, "in1", true, 42)
  EXPECT_EQ(std::get<uint64_t>(entries[0]->key_values[0]), 100U);
  EXPECT_EQ(std::get<bool>(entries[0]->key_values[1]), true);
  EXPECT_EQ(std::get<uint32_t>(entries[0]->order_values[0]), 0U);  // inputs._index
  EXPECT_EQ(std::get<std::string>(entries[0]->order_values[1]), "in1");
  EXPECT_EQ(std::get<bool>(entries[0]->order_values[2]), true);
  EXPECT_EQ(std::get<uint64_t>(entries[0]->order_values[3]), 42U);

  // Entry 1: key=(200, true), order=(0, "in1", true, 42)
  EXPECT_EQ(std::get<uint64_t>(entries[1]->key_values[0]), 200U);
  EXPECT_EQ(std::get<uint32_t>(entries[1]->order_values[0]), 0U);

  // Entry 2: key=(300, true), order=(1, "in2", false, 42)
  EXPECT_EQ(std::get<uint64_t>(entries[2]->key_values[0]), 300U);
  EXPECT_EQ(std::get<uint32_t>(entries[2]->order_values[0]), 1U);
  EXPECT_EQ(std::get<std::string>(entries[2]->order_values[1]), "in2");
  EXPECT_EQ(std::get<bool>(entries[2]->order_values[2]), false);
}

TEST(IndexDerivationTest, NestedRepeatedEmptyOuterArrayProducesNoEntries) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), true);
  // No inputs added.

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  EXPECT_EQ(derived_or->size(), 0U);
}

TEST(IndexDerivationTest, NestedRepeatedEmptyInnerArraySkipsElement) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), true);
  AddInput(message.get(), descriptor, "empty_input", true, {});     // no types
  AddInput(message.get(), descriptor, "has_type", false, {500});

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 42);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  std::vector<const index::DerivedIndexEntry*> entries;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type") {
      entries.push_back(&e);
    }
  }
  // Only 1 row from the second input (first has empty types).
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(std::get<uint64_t>(entries[0]->key_values[0]), 500U);
  EXPECT_EQ(std::get<uint32_t>(entries[0]->order_values[0]), 1U);  // inputs._index = 1
}

TEST(IndexDerivationTest, NestedRepeatedSingleElement) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), false);
  AddInput(message.get(), descriptor, "only", true, {999});

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 1);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  std::vector<const index::DerivedIndexEntry*> entries;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type") {
      entries.push_back(&e);
    }
  }
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(std::get<uint64_t>(entries[0]->key_values[0]), 999U);
  EXPECT_EQ(std::get<bool>(entries[0]->key_values[1]), false);
  EXPECT_EQ(std::get<uint32_t>(entries[0]->order_values[0]), 0U);
  EXPECT_EQ(std::get<std::string>(entries[0]->order_values[1]), "only");
}

TEST(IndexDerivationTest, NestedRepeatedCoveringRefIndex) {
  // The by_input_type_ref index has just key=["inputs.types"] and order=[artifact_id].
  // It should produce one row per distinct (input, type) pair.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), true);
  AddInput(message.get(), descriptor, "a", true, {10, 20});
  AddInput(message.get(), descriptor, "b", false, {30});

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 5);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  std::vector<const index::DerivedIndexEntry*> ref_entries;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type_ref") {
      ref_entries.push_back(&e);
    }
  }
  ASSERT_EQ(ref_entries.size(), 3U);
  EXPECT_EQ(std::get<uint64_t>(ref_entries[0]->key_values[0]), 10U);
  EXPECT_EQ(std::get<uint64_t>(ref_entries[1]->key_values[0]), 20U);
  EXPECT_EQ(std::get<uint64_t>(ref_entries[2]->key_values[0]), 30U);
}

TEST(IndexDerivationTest, NestedRepeatedDeduplicatesSameKeyAndOrder) {
  // If two inner elements produce the same key value and the index has no
  // _index in order, duplicates should be collapsed.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithNestedRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);
  message->GetReflection()->SetBool(message.get(), descriptor->FindFieldByName("is_view_capability"), true);
  // Same type value twice within the same input.
  AddInput(message.get(), descriptor, "dup", true, {100, 100});

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 1);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();

  // by_input_type has inputs._index in order, so both positions are distinct
  // (different order values: _index differs even though key is the same).
  // Actually _index is for inputs, not inputs.types, so both have _index=0.
  // But the by_input_type_ref index has only artifact_id in order, so both
  // produce key=100, order=[1] -- they are true duplicates.
  int ref_count = 0;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type_ref") {
      ++ref_count;
    }
  }
  EXPECT_EQ(ref_count, 1);

  // For by_input_type, both entries have key=(100, true), order=(0, "dup", true, 1).
  // inputs._index=0 for both (same input element), so they are identical -> deduped.
  int type_count = 0;
  for (const auto& e : *derived_or) {
    if (e.key_type == "by_input_type") {
      ++type_count;
    }
  }
  EXPECT_EQ(type_count, 1);
}

// ---------------------------------------------------------------------------
// Validation tests for repeated chain constraints
// ---------------------------------------------------------------------------

const google::protobuf::Descriptor* BuildDescriptorWithBranchingRepeated(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_branching.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* input_msg = file.add_message_type();
  input_msg->set_name("BranchInput");
  auto* input_name = input_msg->add_field();
  input_name->set_name("name");
  input_name->set_number(1);
  input_name->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  input_name->set_type(google::protobuf::FieldDescriptorProto::TYPE_STRING);

  auto* output_msg = file.add_message_type();
  output_msg->set_name("BranchOutput");
  auto* output_type = output_msg->add_field();
  output_type->set_name("type_ref");
  output_type->set_number(1);
  output_type->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
  output_type->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  auto* message = file.add_message_type();
  message->set_name("BranchingArtifact");

  auto* inputs = message->add_field();
  inputs->set_name("inputs");
  inputs->set_number(1);
  inputs->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  inputs->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  inputs->set_type_name("BranchInput");

  auto* outputs = message->add_field();
  outputs->set_name("outputs");
  outputs->set_number(2);
  outputs->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  outputs->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  outputs->set_type_name("BranchOutput");

  // Index with branching repeated paths: inputs.name + outputs.type_ref
  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("branching");
  index->add_key("inputs.name");
  index->add_key("outputs.type_ref");
  auto* order = index->add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const auto* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("BranchingArtifact");
}

TEST(IndexDerivationTest, RejectsBranchingRepeatedPaths) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithBranchingRepeated(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 1);
  ASSERT_FALSE(derived_or.ok());
  EXPECT_EQ(derived_or.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(std::string(derived_or.status().message()).find("INVALID_INDEX_DEFINITION"), std::string::npos);
}

const google::protobuf::Descriptor* BuildDescriptorWithVirtualIndexDeep(google::protobuf::DescriptorPool* pool) {
  google::protobuf::FileDescriptorProto file;
  file.set_name("index_derivation_virtual_deep.proto");
  file.set_syntax("proto3");
  file.set_package("artifact_system.testing");
  file.add_dependency("artifact_options.proto");

  auto* input_msg = file.add_message_type();
  input_msg->set_name("DeepInput");
  auto* input_types = input_msg->add_field();
  input_types->set_name("types");
  input_types->set_number(1);
  input_types->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  input_types->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);

  auto* message = file.add_message_type();
  message->set_name("VirtualDeepArtifact");

  auto* inputs = message->add_field();
  inputs->set_name("inputs");
  inputs->set_number(1);
  inputs->set_label(google::protobuf::FieldDescriptorProto::LABEL_REPEATED);
  inputs->set_type(google::protobuf::FieldDescriptorProto::TYPE_MESSAGE);
  inputs->set_type_name("DeepInput");

  // Index with inputs.types._index in order.
  auto* index = message->mutable_options()->AddExtension(artifact_system::indexes);
  index->set_key_type("by_type_with_deep_index");
  index->add_key("inputs.types");
  auto* o1 = index->add_order();
  o1->set_field("inputs._index");
  o1->set_direction(artifact_system::OrderDefinition::ASCENDING);
  auto* o2 = index->add_order();
  o2->set_field("inputs.types._index");
  o2->set_direction(artifact_system::OrderDefinition::ASCENDING);
  auto* o3 = index->add_order();
  o3->set_field("artifact_id");
  o3->set_direction(artifact_system::OrderDefinition::ASCENDING);

  const auto* built = pool->BuildFile(file);
  if (built == nullptr) {
    return nullptr;
  }
  return built->FindMessageTypeByName("VirtualDeepArtifact");
}

TEST(IndexDerivationTest, VirtualIndexDeepField) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildDescriptorWithVirtualIndexDeep(&pool);
  ASSERT_NE(descriptor, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  auto message = NewMessage(descriptor, &factory);
  ASSERT_NE(message, nullptr);

  const auto* inputs_fd = descriptor->FindFieldByName("inputs");
  const auto* refl = message->GetReflection();
  auto* in0 = refl->AddMessage(message.get(), inputs_fd);
  in0->GetReflection()->AddUInt64(in0, in0->GetDescriptor()->FindFieldByName("types"), 10);
  in0->GetReflection()->AddUInt64(in0, in0->GetDescriptor()->FindFieldByName("types"), 20);
  auto* in1 = refl->AddMessage(message.get(), inputs_fd);
  in1->GetReflection()->AddUInt64(in1, in1->GetDescriptor()->FindFieldByName("types"), 30);

  auto derived_or = index::DeriveIndexEntries(*descriptor, *message, 99);
  ASSERT_TRUE(derived_or.ok()) << derived_or.status();
  ASSERT_EQ(derived_or->size(), 3U);

  // Entry 0: key=10, order=(inputs._index=0, types._index=0, aid=99)
  EXPECT_EQ(std::get<uint64_t>(derived_or->at(0).key_values[0]), 10U);
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(0).order_values[0]), 0U);  // inputs._index
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(0).order_values[1]), 0U);  // types._index

  // Entry 1: key=20, order=(inputs._index=0, types._index=1, aid=99)
  EXPECT_EQ(std::get<uint64_t>(derived_or->at(1).key_values[0]), 20U);
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(1).order_values[0]), 0U);
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(1).order_values[1]), 1U);

  // Entry 2: key=30, order=(inputs._index=1, types._index=0, aid=99)
  EXPECT_EQ(std::get<uint64_t>(derived_or->at(2).key_values[0]), 30U);
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(2).order_values[0]), 1U);
  EXPECT_EQ(std::get<uint32_t>(derived_or->at(2).order_values[1]), 0U);
}

} // namespace artifact_system::testing

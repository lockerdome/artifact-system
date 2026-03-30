#include "registry/schema_compatibility.h"

#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using google::protobuf::DescriptorProto;
using google::protobuf::FieldDescriptorProto;
using google::protobuf::FileDescriptorProto;
using google::protobuf::FileDescriptorSet;
using google::protobuf::OneofDescriptorProto;
using registry::CheckSchemaCompatibility;
using registry::SchemaViolation;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FileDescriptorSet MakeFds(const FileDescriptorProto& file) {
  FileDescriptorSet fds;
  *fds.add_file() = file;
  return fds;
}

// Create a minimal proto3 FileDescriptorProto with one message.
FileDescriptorProto MakeFileProto(const std::string& msg_name) {
  FileDescriptorProto file;
  file.set_name("test.proto");
  file.set_syntax("proto3");
  auto* msg = file.add_message_type();
  msg->set_name(msg_name);
  return file;
}

void AddField(DescriptorProto* msg, const std::string& name, int number, FieldDescriptorProto::Type type,
              FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL) {
  auto* f = msg->add_field();
  f->set_name(name);
  f->set_number(number);
  f->set_type(type);
  f->set_label(label);
}

void AddMessageField(DescriptorProto* msg, const std::string& name, int number, const std::string& type_name,
                     FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL) {
  auto* f = msg->add_field();
  f->set_name(name);
  f->set_number(number);
  f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  f->set_type_name(type_name);
  f->set_label(label);
}

// Add a nested message type to a parent message and return a pointer to it.
DescriptorProto* AddNestedMessage(DescriptorProto* parent, const std::string& name) {
  auto* nested = parent->add_nested_type();
  nested->set_name(name);
  return nested;
}

// Add a oneof to a message and return its index.
int AddOneof(DescriptorProto* msg, const std::string& name) {
  auto* oneof = msg->add_oneof_decl();
  oneof->set_name(name);
  return msg->oneof_decl_size() - 1;
}

// Add a field that belongs to a oneof (by oneof index).
void AddOneofField(DescriptorProto* msg, const std::string& name, int number, FieldDescriptorProto::Type type, int oneof_index) {
  auto* f = msg->add_field();
  f->set_name(name);
  f->set_number(number);
  f->set_type(type);
  f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  f->set_oneof_index(oneof_index);
}

// Check whether any violation description contains the given substring.
bool HasViolationContaining(const std::vector<SchemaViolation>& violations, const std::string& substring) {
  for (const auto& v : violations) {
    if (v.description.find(substring) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(SchemaCompatibilityTest, IdenticalSchemas) {
  auto file = MakeFileProto("TestMsg");
  auto* msg = file.mutable_message_type(0);
  AddField(msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);
  AddField(msg, "name", 2, FieldDescriptorProto::TYPE_STRING);

  auto violations = CheckSchemaCompatibility(MakeFds(file), MakeFds(file), "TestMsg");
  EXPECT_TRUE(violations.empty());
}

TEST(SchemaCompatibilityTest, CompatibleSchemasAddField) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);
  AddField(new_msg, "name", 2, FieldDescriptorProto::TYPE_STRING);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_TRUE(violations.empty());
}

TEST(SchemaCompatibilityTest, FieldRemoval) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);
  AddField(old_msg, "name", 2, FieldDescriptorProto::TYPE_STRING);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);
  // "name" field removed.

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "was removed"));
  EXPECT_NE(violations[0].subject.find("name"), std::string::npos);
}

TEST(SchemaCompatibilityTest, FieldTypeChange) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "id", 1, FieldDescriptorProto::TYPE_STRING);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "changed type"));
  EXPECT_TRUE(HasViolationContaining(violations, "uint64"));
  EXPECT_TRUE(HasViolationContaining(violations, "string"));
}

TEST(SchemaCompatibilityTest, LabelChange) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "tags", 1, FieldDescriptorProto::TYPE_STRING, FieldDescriptorProto::LABEL_OPTIONAL);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "tags", 1, FieldDescriptorProto::TYPE_STRING, FieldDescriptorProto::LABEL_REPEATED);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "changed label"));
  EXPECT_TRUE(HasViolationContaining(violations, "optional"));
  EXPECT_TRUE(HasViolationContaining(violations, "repeated"));
}

TEST(SchemaCompatibilityTest, FieldNumberReassignment) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "alpha", 1, FieldDescriptorProto::TYPE_STRING);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "beta", 1, FieldDescriptorProto::TYPE_STRING);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "changed name"));
  EXPECT_TRUE(HasViolationContaining(violations, "alpha"));
  EXPECT_TRUE(HasViolationContaining(violations, "beta"));
}

TEST(SchemaCompatibilityTest, OneofRemoval) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  int oneof_idx = AddOneof(old_msg, "value");
  AddOneofField(old_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, oneof_idx);
  AddOneofField(old_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64, oneof_idx);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  // Fields remain but the oneof is gone — they are now independent fields.
  AddField(new_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING);
  AddField(new_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  // Should report the oneof removal and that fields moved out of the oneof.
  EXPECT_FALSE(violations.empty());
  EXPECT_TRUE(HasViolationContaining(violations, "oneof"));
  EXPECT_TRUE(HasViolationContaining(violations, "was removed") || HasViolationContaining(violations, "moved out"));
}

TEST(SchemaCompatibilityTest, OneofFieldRemoval) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  int oneof_idx = AddOneof(old_msg, "value");
  AddOneofField(old_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, oneof_idx);
  AddOneofField(old_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64, oneof_idx);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  int new_oneof_idx = AddOneof(new_msg, "value");
  AddOneofField(new_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, new_oneof_idx);
  // "int_val" removed entirely.

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_FALSE(violations.empty());
  EXPECT_TRUE(HasViolationContaining(violations, "was removed"));
  EXPECT_TRUE(HasViolationContaining(violations, "int_val"));
}

TEST(SchemaCompatibilityTest, OneofFieldMovedOut) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  int oneof_idx = AddOneof(old_msg, "value");
  AddOneofField(old_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, oneof_idx);
  AddOneofField(old_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64, oneof_idx);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  int new_oneof_idx = AddOneof(new_msg, "value");
  AddOneofField(new_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, new_oneof_idx);
  // "int_val" still exists but is no longer in the oneof.
  AddField(new_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_FALSE(violations.empty());
  EXPECT_TRUE(HasViolationContaining(violations, "moved out of oneof"));
}

TEST(SchemaCompatibilityTest, AddingOneofFieldIsOK) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  int oneof_idx = AddOneof(old_msg, "value");
  AddOneofField(old_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, oneof_idx);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  int new_oneof_idx = AddOneof(new_msg, "value");
  AddOneofField(new_msg, "str_val", 1, FieldDescriptorProto::TYPE_STRING, new_oneof_idx);
  AddOneofField(new_msg, "int_val", 2, FieldDescriptorProto::TYPE_INT64, new_oneof_idx);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_TRUE(violations.empty());
}

TEST(SchemaCompatibilityTest, NestedMessageFieldRemoval) {
  auto file_old = MakeFileProto("Outer");
  auto* outer_old = file_old.mutable_message_type(0);
  auto* inner_old = AddNestedMessage(outer_old, "Inner");
  AddField(inner_old, "x", 1, FieldDescriptorProto::TYPE_INT32);
  AddField(inner_old, "y", 2, FieldDescriptorProto::TYPE_INT32);
  AddMessageField(outer_old, "inner", 1, "Inner");

  auto file_new = MakeFileProto("Outer");
  auto* outer_new = file_new.mutable_message_type(0);
  auto* inner_new = AddNestedMessage(outer_new, "Inner");
  AddField(inner_new, "x", 1, FieldDescriptorProto::TYPE_INT32);
  // "y" removed from Inner.
  AddMessageField(outer_new, "inner", 1, "Inner");

  auto violations = CheckSchemaCompatibility(MakeFds(file_old), MakeFds(file_new), "Outer");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "was removed"));
  EXPECT_TRUE(HasViolationContaining(violations, "y"));
}

TEST(SchemaCompatibilityTest, NestedMessageTypeChange) {
  auto file_old = MakeFileProto("Outer");
  auto* outer_old = file_old.mutable_message_type(0);
  auto* inner_old = AddNestedMessage(outer_old, "Inner");
  AddField(inner_old, "count", 1, FieldDescriptorProto::TYPE_INT32);
  AddMessageField(outer_old, "inner", 1, "Inner");

  auto file_new = MakeFileProto("Outer");
  auto* outer_new = file_new.mutable_message_type(0);
  auto* inner_new = AddNestedMessage(outer_new, "Inner");
  AddField(inner_new, "count", 1, FieldDescriptorProto::TYPE_STRING);
  AddMessageField(outer_new, "inner", 1, "Inner");

  auto violations = CheckSchemaCompatibility(MakeFds(file_old), MakeFds(file_new), "Outer");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "changed type"));
  EXPECT_TRUE(HasViolationContaining(violations, "inner.count"));
}

TEST(SchemaCompatibilityTest, DeepNestingThreePlusLevels) {
  // A -> B -> C -> D hierarchy.  Change a field in D.
  auto file_old = MakeFileProto("A");
  auto* a_old = file_old.mutable_message_type(0);
  auto* b_old = AddNestedMessage(a_old, "B");
  auto* c_old = AddNestedMessage(b_old, "C");
  auto* d_old = AddNestedMessage(c_old, "D");
  AddField(d_old, "value", 1, FieldDescriptorProto::TYPE_INT64);
  AddMessageField(c_old, "d", 1, "D");
  AddMessageField(b_old, "c", 1, "C");
  AddMessageField(a_old, "b", 1, "B");

  auto file_new = MakeFileProto("A");
  auto* a_new = file_new.mutable_message_type(0);
  auto* b_new = AddNestedMessage(a_new, "B");
  auto* c_new = AddNestedMessage(b_new, "C");
  auto* d_new = AddNestedMessage(c_new, "D");
  AddField(d_new, "value", 1, FieldDescriptorProto::TYPE_STRING); // changed
  AddMessageField(c_new, "d", 1, "D");
  AddMessageField(b_new, "c", 1, "C");
  AddMessageField(a_new, "b", 1, "B");

  auto violations = CheckSchemaCompatibility(MakeFds(file_old), MakeFds(file_new), "A");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "changed type"));
  // The path should reflect the full nesting.
  EXPECT_TRUE(HasViolationContaining(violations, "b.c.d.value"));
}

TEST(SchemaCompatibilityTest, MultipleViolations) {
  auto old_file = MakeFileProto("TestMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);
  AddField(old_msg, "name", 2, FieldDescriptorProto::TYPE_STRING);
  AddField(old_msg, "tags", 3, FieldDescriptorProto::TYPE_STRING, FieldDescriptorProto::LABEL_OPTIONAL);

  auto new_file = MakeFileProto("TestMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  // "id" removed, "name" type changed, "tags" label changed.
  AddField(new_msg, "name", 2, FieldDescriptorProto::TYPE_INT32);
  AddField(new_msg, "tags", 3, FieldDescriptorProto::TYPE_STRING, FieldDescriptorProto::LABEL_REPEATED);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "TestMsg");
  EXPECT_GE(violations.size(), 3u);
  EXPECT_TRUE(HasViolationContaining(violations, "was removed"));
  EXPECT_TRUE(HasViolationContaining(violations, "changed type"));
  EXPECT_TRUE(HasViolationContaining(violations, "changed label"));
}

TEST(SchemaCompatibilityTest, TypeNotFoundInOldSchema) {
  auto file = MakeFileProto("TestMsg");
  auto* msg = file.mutable_message_type(0);
  AddField(msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);

  auto violations = CheckSchemaCompatibility(MakeFds(file), MakeFds(file), "NoSuchType");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "not found"));
  EXPECT_TRUE(HasViolationContaining(violations, "NoSuchType"));
}

TEST(SchemaCompatibilityTest, TypeNotFoundInNewSchema) {
  auto old_file = MakeFileProto("OldMsg");
  auto* old_msg = old_file.mutable_message_type(0);
  AddField(old_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);

  auto new_file = MakeFileProto("NewMsg");
  auto* new_msg = new_file.mutable_message_type(0);
  AddField(new_msg, "id", 1, FieldDescriptorProto::TYPE_UINT64);

  auto violations = CheckSchemaCompatibility(MakeFds(old_file), MakeFds(new_file), "OldMsg");
  EXPECT_EQ(violations.size(), 1);
  EXPECT_TRUE(HasViolationContaining(violations, "not found"));
  EXPECT_TRUE(HasViolationContaining(violations, "new schema"));
}

} // namespace
} // namespace artifact_system::testing

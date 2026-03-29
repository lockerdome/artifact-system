#include "registry/schema_compatibility.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/strings/string_view.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor_database.h"

namespace artifact_system::registry {
namespace {

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::DescriptorPoolDatabase;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptorSet;
using google::protobuf::MergedDescriptorDatabase;
using google::protobuf::OneofDescriptor;
using google::protobuf::SimpleDescriptorDatabase;

enum class FieldLabel { kOptional, kRequired, kRepeated };

FieldLabel GetFieldLabel(const FieldDescriptor* field) {
  if (field->is_required())
    return FieldLabel::kRequired;
  if (field->is_repeated())
    return FieldLabel::kRepeated;
  return FieldLabel::kOptional;
}

std::string LabelName(FieldLabel label) {
  switch (label) {
  case FieldLabel::kOptional:
    return "optional";
  case FieldLabel::kRequired:
    return "required";
  case FieldLabel::kRepeated:
    return "repeated";
  }
  return "unknown";
}

std::string TypeName(FieldDescriptor::Type type) {
  return std::string(FieldDescriptor::TypeName(type));
}

// Helper to convert absl::string_view (returned by protobuf name accessors) to std::string.
std::string S(absl::string_view sv) {
  return std::string(sv);
}

// Recursively compare two message descriptors for backward-incompatible changes.
void CompareMessages(const Descriptor* old_msg, const Descriptor* new_msg, const std::string& path_prefix, std::unordered_set<std::string>& visited,
                     std::vector<SchemaViolation>& violations) {
  std::string full_name = S(old_msg->full_name());
  if (visited.count(full_name))
    return;
  visited.insert(full_name);

  // Index new message fields by field number for lookup.
  std::unordered_map<int, const FieldDescriptor*> new_fields_by_number;
  for (int i = 0; i < new_msg->field_count(); ++i) {
    new_fields_by_number[new_msg->field(i)->number()] = new_msg->field(i);
  }

  // Check each old field exists and is compatible in the new message.
  for (int i = 0; i < old_msg->field_count(); ++i) {
    const FieldDescriptor* old_field = old_msg->field(i);
    std::string field_path = path_prefix + S(old_field->name());

    auto it = new_fields_by_number.find(old_field->number());
    if (it == new_fields_by_number.end()) {
      violations.push_back({"field '" + field_path + "' (number " + std::to_string(old_field->number()) + ") was removed", "field: " + field_path});
      continue;
    }

    const FieldDescriptor* new_field = it->second;

    // Field number reassignment: same number, different name.
    if (old_field->name() != new_field->name()) {
      violations.push_back(
          {"field number " + std::to_string(old_field->number()) + " changed name from '" + S(old_field->name()) + "' to '" + S(new_field->name()) + "'",
           "field_number: " + std::to_string(old_field->number())});
      continue;
    }

    // Type change.
    if (old_field->type() != new_field->type()) {
      violations.push_back(
          {"field '" + field_path + "' changed type from " + TypeName(old_field->type()) + " to " + TypeName(new_field->type()), "field: " + field_path});
    }

    // Label change (optional/required/repeated).
    FieldLabel old_label = GetFieldLabel(old_field);
    FieldLabel new_label = GetFieldLabel(new_field);
    if (old_label != new_label) {
      violations.push_back({"field '" + field_path + "' changed label from " + LabelName(old_label) + " to " + LabelName(new_label), "field: " + field_path});
    }

    // Recursive check for nested message types.
    if (old_field->type() == FieldDescriptor::TYPE_MESSAGE && new_field->type() == FieldDescriptor::TYPE_MESSAGE) {
      CompareMessages(old_field->message_type(), new_field->message_type(), field_path + ".", visited, violations);
    }
  }

  // Oneof checks: each old real oneof must still exist, and its fields must remain.
  // Use real_oneof_decl_count/real_oneof_decl to skip synthetic oneofs.
  for (int i = 0; i < old_msg->real_oneof_decl_count(); ++i) {
    const OneofDescriptor* old_oneof = old_msg->real_oneof_decl(i);

    std::string oneof_name = S(old_oneof->name());
    const OneofDescriptor* new_oneof = new_msg->FindOneofByName(oneof_name);
    if (!new_oneof) {
      violations.push_back({"oneof '" + path_prefix + oneof_name + "' was removed", "oneof: " + path_prefix + oneof_name});
      continue;
    }

    // Index new oneof fields by number.
    std::unordered_set<int> new_oneof_field_numbers;
    for (int j = 0; j < new_oneof->field_count(); ++j) {
      new_oneof_field_numbers.insert(new_oneof->field(j)->number());
    }

    // Each old oneof field must still be in the same oneof.
    for (int j = 0; j < old_oneof->field_count(); ++j) {
      const FieldDescriptor* old_oneof_field = old_oneof->field(j);
      const FieldDescriptor* new_field = new_msg->FindFieldByNumber(old_oneof_field->number());

      if (!new_field) {
        continue;
      }

      if (!new_field->containing_oneof() || new_field->containing_oneof()->name() != old_oneof->name()) {
        std::string fname = S(old_oneof_field->name());
        violations.push_back({"field '" + path_prefix + fname + "' was moved out of oneof '" + oneof_name + "'", "field: " + path_prefix + fname});
      }
    }
  }
}

} // namespace

std::vector<SchemaViolation> CheckSchemaCompatibility(const FileDescriptorSet& old_descriptor_set, const FileDescriptorSet& new_descriptor_set,
                                                      const std::string& type_name) {
  std::vector<SchemaViolation> violations;

  // Build pools with generated_pool as underlay for well-known types.
  DescriptorPoolDatabase generated_db(*DescriptorPool::generated_pool());

  SimpleDescriptorDatabase old_db, new_db;
  for (const auto& file : old_descriptor_set.file())
    old_db.Add(file);
  for (const auto& file : new_descriptor_set.file())
    new_db.Add(file);

  MergedDescriptorDatabase old_merged(&generated_db, &old_db);
  MergedDescriptorDatabase new_merged(&generated_db, &new_db);
  DescriptorPool old_pool(&old_merged);
  DescriptorPool new_pool(&new_merged);

  const Descriptor* old_msg = old_pool.FindMessageTypeByName(type_name);
  if (!old_msg) {
    violations.push_back({"type '" + type_name + "' not found in old schema", "type: " + type_name});
    return violations;
  }

  const Descriptor* new_msg = new_pool.FindMessageTypeByName(type_name);
  if (!new_msg) {
    violations.push_back({"type '" + type_name + "' not found in new schema", "type: " + type_name});
    return violations;
  }

  std::unordered_set<std::string> visited;
  CompareMessages(old_msg, new_msg, /*path_prefix=*/"", visited, violations);
  return violations;
}

} // namespace artifact_system::registry

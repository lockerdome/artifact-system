#include "index/index_schema_generator.h"

#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact/field_path.h"

namespace artifact_system::index {
namespace {

using Type = google::protobuf::FieldDescriptorProto::Type;
using Label = google::protobuf::FieldDescriptorProto::Label;

std::string SanitizeForIdentifier(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (const char c : input) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

std::string MakeUniqueFieldName(std::string base, std::set<std::string>* used_names) {
  if (!used_names->contains(base)) {
    used_names->insert(base);
    return base;
  }
  for (size_t suffix = 2;; ++suffix) {
    const std::string candidate = absl::StrCat(base, "_", suffix);
    if (!used_names->contains(candidate)) {
      used_names->insert(candidate);
      return candidate;
    }
  }
}

// Check if a field path ends with the virtual _index segment.
bool IsVirtualIndexPath(std::string_view path) {
  return path.size() >= 6 && path.substr(path.size() - 6) == "_index" && (path.size() == 6 || path[path.size() - 7] == '.');
}

// Resolve a field path for index schema generation. Returns the leaf
// FieldDescriptor, or nullptr for virtual _index paths.
absl::StatusOr<const google::protobuf::FieldDescriptor*> ResolveIndexFieldLeaf(const google::protobuf::Descriptor& root, std::string_view path) {
  auto resolved_or = artifact::ResolveIndexFieldPath(root, path);
  if (!resolved_or.ok()) {
    return resolved_or.status();
  }
  return artifact::IndexFieldPathLeaf(*resolved_or);
}

Type ToFieldType(const google::protobuf::FieldDescriptor& field) {
  return static_cast<Type>(field.type());
}

std::string ToTypeName(const google::protobuf::Descriptor& descriptor) {
  return absl::StrCat(".", descriptor.full_name());
}

std::string ToTypeName(const google::protobuf::EnumDescriptor& descriptor) {
  return absl::StrCat(".", descriptor.full_name());
}

void AddScalarField(google::protobuf::DescriptorProto* message, const std::string& name, int32_t number, const google::protobuf::FieldDescriptor& source_field,
                    Label label_override = Label::FieldDescriptorProto_Label_LABEL_OPTIONAL, std::optional<int> oneof_index = std::nullopt) {
  auto* field = message->add_field();
  field->set_name(name);
  field->set_number(number);
  field->set_label(label_override);
  field->set_type(ToFieldType(source_field));
  if (oneof_index.has_value()) {
    field->set_proto3_optional(true);
    field->set_oneof_index(*oneof_index);
  }
  if (source_field.type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
    field->set_type_name(ToTypeName(*source_field.enum_type()));
  }
}

} // namespace

absl::StatusOr<GeneratedIndexSchema> GenerateIndexSchema(const artifact_system::IndexDefinition& index_definition,
                                                         const google::protobuf::Descriptor& parent_descriptor) {
  if (index_definition.key_type().empty()) {
    return absl::InvalidArgumentError("index_definition.key_type must not be empty");
  }
  if (index_definition.order().empty()) {
    return absl::InvalidArgumentError("index_definition.order must contain at least one field");
  }
  int artifact_id_order_count = 0;
  for (const auto& order : index_definition.order()) {
    if (order.direction() == artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED) {
      return absl::InvalidArgumentError("index_definition.order direction must be ASCENDING or DESCENDING");
    }
    if (order.field() == "artifact_id") {
      ++artifact_id_order_count;
    }
  }
  if (artifact_id_order_count != 1) {
    return absl::InvalidArgumentError("index_definition.order must include artifact_id");
  }

  const std::string suffix = SanitizeForIdentifier(absl::StrCat(parent_descriptor.full_name(), "_", index_definition.key_type()));
  const std::string key_name = absl::StrCat("IndexKey_", suffix);
  const std::string value_name = absl::StrCat("IndexValue_", suffix);
  const std::string index_name = absl::StrCat("Index_", suffix);

  google::protobuf::FileDescriptorProto file_proto;
  file_proto.set_name(absl::StrCat("generated/index/", suffix, ".proto"));
  file_proto.set_package("artifact_system.generated");
  file_proto.set_syntax("proto3");
  std::set<std::string> dependency_names;
  dependency_names.insert(std::string(parent_descriptor.file()->name()));

  google::protobuf::DescriptorProto* key_message = file_proto.add_message_type();
  key_message->set_name(key_name);
  std::set<std::string> key_names;
  for (int i = 0; i < index_definition.key_size(); ++i) {
    const std::string& key_path = index_definition.key(i);
    const std::string field_name = MakeUniqueFieldName(SanitizeForIdentifier(key_path), &key_names);
    auto* oneof = key_message->add_oneof_decl();
    oneof->set_name(absl::StrCat("_", field_name));

    if (IsVirtualIndexPath(key_path)) {
      auto* field = key_message->add_field();
      field->set_name(field_name);
      field->set_number(i + 1);
      field->set_label(Label::FieldDescriptorProto_Label_LABEL_OPTIONAL);
      field->set_type(Type::FieldDescriptorProto_Type_TYPE_UINT32);
      field->set_proto3_optional(true);
      field->set_oneof_index(key_message->oneof_decl_size() - 1);
      continue;
    }

    auto field_or = ResolveIndexFieldLeaf(parent_descriptor, key_path);
    if (!field_or.ok()) {
      return field_or.status();
    }
    const google::protobuf::FieldDescriptor* source = *field_or;
    dependency_names.insert(std::string(source->file()->name()));
    if (source->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
      dependency_names.insert(std::string(source->enum_type()->file()->name()));
    }
    AddScalarField(key_message, field_name, i + 1, *source, Label::FieldDescriptorProto_Label_LABEL_OPTIONAL, key_message->oneof_decl_size() - 1);
  }

  google::protobuf::DescriptorProto* value_message = file_proto.add_message_type();
  value_message->set_name(value_name);
  auto* row_count_field = value_message->add_field();
  row_count_field->set_name("row_count");
  row_count_field->set_number(1);
  row_count_field->set_label(Label::FieldDescriptorProto_Label_LABEL_OPTIONAL);
  row_count_field->set_type(Type::FieldDescriptorProto_Type_TYPE_UINT32);

  std::set<std::string> value_names;
  value_names.insert("row_count");
  for (int i = 0; i < index_definition.order_size(); ++i) {
    const std::string field_path = index_definition.order(i).field();
    auto* field = value_message->add_field();
    field->set_name(MakeUniqueFieldName(SanitizeForIdentifier(field_path), &value_names));
    field->set_number(i + 2);
    field->set_label(Label::FieldDescriptorProto_Label_LABEL_REPEATED);
    if (field_path == "artifact_id") {
      field->set_type(Type::FieldDescriptorProto_Type_TYPE_UINT64);
      continue;
    }
    if (IsVirtualIndexPath(field_path)) {
      field->set_type(Type::FieldDescriptorProto_Type_TYPE_UINT32);
      continue;
    }
    auto source_field_or = ResolveIndexFieldLeaf(parent_descriptor, field_path);
    if (!source_field_or.ok()) {
      return source_field_or.status();
    }
    const auto* source_field = *source_field_or;
    dependency_names.insert(std::string(source_field->file()->name()));
    field->set_type(ToFieldType(*source_field));
    if (source_field->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
      dependency_names.insert(std::string(source_field->enum_type()->file()->name()));
      field->set_type_name(ToTypeName(*source_field->enum_type()));
    }
  }

  for (const std::string& dependency_name : dependency_names) {
    file_proto.add_dependency(dependency_name);
  }

  google::protobuf::DescriptorProto* index_message = file_proto.add_message_type();
  index_message->set_name(index_name);

  auto* key_field = index_message->add_field();
  key_field->set_name("key");
  key_field->set_number(1);
  key_field->set_label(Label::FieldDescriptorProto_Label_LABEL_OPTIONAL);
  key_field->set_type(Type::FieldDescriptorProto_Type_TYPE_MESSAGE);
  key_field->set_type_name(absl::StrCat(".artifact_system.generated.", key_name));

  auto* value_field = index_message->add_field();
  value_field->set_name("value");
  value_field->set_number(2);
  value_field->set_label(Label::FieldDescriptorProto_Label_LABEL_OPTIONAL);
  value_field->set_type(Type::FieldDescriptorProto_Type_TYPE_MESSAGE);
  value_field->set_type_name(absl::StrCat(".artifact_system.generated.", value_name));

  auto pool = std::make_shared<google::protobuf::DescriptorPool>(parent_descriptor.file()->pool());
  const google::protobuf::FileDescriptor* built = pool->BuildFile(file_proto);
  if (built == nullptr) {
    return absl::InvalidArgumentError("failed to build generated index schema descriptor");
  }

  GeneratedIndexSchema schema;
  schema.pool = std::move(pool);
  schema.file_descriptor = built;
  schema.key_descriptor = built->FindMessageTypeByName(key_name);
  schema.value_descriptor = built->FindMessageTypeByName(value_name);
  schema.index_descriptor = built->FindMessageTypeByName(index_name);
  if (schema.key_descriptor == nullptr || schema.value_descriptor == nullptr || schema.index_descriptor == nullptr) {
    return absl::InternalError("generated schema descriptors missing expected messages");
  }

  for (int i = 0; i < index_definition.order_size(); ++i) {
    const auto* field = schema.value_descriptor->FindFieldByNumber(i + 2);
    if (field == nullptr) {
      return absl::InternalError("generated value descriptor missing order field");
    }
    schema.value_fields.push_back(field);
  }

  return schema;
}

} // namespace artifact_system::index

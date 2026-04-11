#include "index/index_derivation.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "artifact/field_path.h"
#include "artifact_options.pb.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"

namespace artifact_system::index {
namespace {

// Alias for the shared field path resolver.
const auto& ResolveFieldPath = artifact::ResolveFieldPath;

absl::StatusOr<const google::protobuf::Message*> ResolveLeafParentMessage(const google::protobuf::Message& root,
                                                                          const std::vector<const google::protobuf::FieldDescriptor*>& path) {
  const google::protobuf::Message* current = &root;
  for (size_t i = 0; i + 1 < path.size(); ++i) {
    const auto* field = path[i];
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      return absl::InvalidArgumentError("non-message intermediate field in path");
    }
    if (field->is_repeated()) {
      return absl::InvalidArgumentError("repeated message intermediate fields are not supported in path");
    }
    const auto* reflection = current->GetReflection();
    if (field->has_presence() && !reflection->HasField(*current, field)) {
      return static_cast<const google::protobuf::Message*>(nullptr);
    }
    current = &reflection->GetMessage(*current, field);
  }
  return current;
}

absl::StatusOr<IndexCell> ScalarValueForField(const google::protobuf::Message& message, const google::protobuf::FieldDescriptor& field, int repeated_index) {
  const auto* reflection = message.GetReflection();
  using Field = google::protobuf::FieldDescriptor;
  const bool repeated = field.is_repeated();

  switch (field.type()) {
  case Field::TYPE_INT32:
  case Field::TYPE_SINT32:
  case Field::TYPE_SFIXED32:
    return repeated ? IndexCell(reflection->GetRepeatedInt32(message, &field, repeated_index)) : IndexCell(reflection->GetInt32(message, &field));
  case Field::TYPE_ENUM:
    return repeated ? IndexCell(reflection->GetRepeatedEnumValue(message, &field, repeated_index)) : IndexCell(reflection->GetEnumValue(message, &field));
  case Field::TYPE_UINT32:
  case Field::TYPE_FIXED32:
    return repeated ? IndexCell(reflection->GetRepeatedUInt32(message, &field, repeated_index)) : IndexCell(reflection->GetUInt32(message, &field));
  case Field::TYPE_INT64:
  case Field::TYPE_SINT64:
  case Field::TYPE_SFIXED64:
    return repeated ? IndexCell(reflection->GetRepeatedInt64(message, &field, repeated_index)) : IndexCell(reflection->GetInt64(message, &field));
  case Field::TYPE_UINT64:
  case Field::TYPE_FIXED64:
    return repeated ? IndexCell(reflection->GetRepeatedUInt64(message, &field, repeated_index)) : IndexCell(reflection->GetUInt64(message, &field));
  case Field::TYPE_BOOL:
    return repeated ? IndexCell(reflection->GetRepeatedBool(message, &field, repeated_index)) : IndexCell(reflection->GetBool(message, &field));
  case Field::TYPE_FLOAT: {
    const float value = repeated ? reflection->GetRepeatedFloat(message, &field, repeated_index) : reflection->GetFloat(message, &field);
    if (std::isnan(value)) {
      return absl::InvalidArgumentError(absl::StrCat("NAN_IN_INDEXED_FIELD: ", field.full_name()));
    }
    return IndexCell(value == 0.0F ? 0.0F : value);
  }
  case Field::TYPE_DOUBLE: {
    const double value = repeated ? reflection->GetRepeatedDouble(message, &field, repeated_index) : reflection->GetDouble(message, &field);
    if (std::isnan(value)) {
      return absl::InvalidArgumentError(absl::StrCat("NAN_IN_INDEXED_FIELD: ", field.full_name()));
    }
    return IndexCell(value == 0.0 ? 0.0 : value);
  }
  case Field::TYPE_STRING:
    return repeated ? IndexCell(reflection->GetRepeatedString(message, &field, repeated_index)) : IndexCell(reflection->GetString(message, &field));
  case Field::TYPE_BYTES:
    return repeated ? IndexCell(BytesValue{reflection->GetRepeatedString(message, &field, repeated_index)})
                    : IndexCell(BytesValue{reflection->GetString(message, &field)});
  default:
    return absl::InvalidArgumentError(absl::StrCat("unsupported indexed field type: ", field.full_name()));
  }
}

void AppendUint32Le(uint32_t value, std::vector<uint8_t>* out) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

void AppendUint64Le(uint64_t value, std::vector<uint8_t>* out) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 32U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 40U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 48U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 56U) & 0xFFU));
}

bool IsMinimalVarintEncoding(const std::vector<uint8_t>& encoded) {
  if (encoded.empty() || encoded.size() > 10) {
    return false;
  }
  uint64_t value = 0;
  int shift = 0;
  for (size_t i = 0; i < encoded.size(); ++i) {
    const uint8_t byte = encoded[i];
    const bool is_last = (i + 1 == encoded.size());
    if (is_last && (byte & 0x80U) != 0) {
      return false;
    }
    if (!is_last && (byte & 0x80U) == 0) {
      return false;
    }
    value |= static_cast<uint64_t>(byte & 0x7FU) << shift;
    shift += 7;
  }
  size_t minimal_len = 1;
  while (value >= 0x80U) {
    value >>= 7;
    ++minimal_len;
  }
  return minimal_len == encoded.size();
}

absl::Status EncodeIndexCell(const IndexCell& cell, std::vector<uint8_t>* out) {
  if (std::holds_alternative<int32_t>(cell)) {
    AppendUint32Le(static_cast<uint32_t>(std::get<int32_t>(cell)), out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<uint32_t>(cell)) {
    AppendUint32Le(std::get<uint32_t>(cell), out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<int64_t>(cell)) {
    AppendUint64Le(static_cast<uint64_t>(std::get<int64_t>(cell)), out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<uint64_t>(cell)) {
    AppendUint64Le(std::get<uint64_t>(cell), out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<bool>(cell)) {
    out->push_back(std::get<bool>(cell) ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0));
    return absl::OkStatus();
  }
  if (std::holds_alternative<float>(cell)) {
    const float value = std::get<float>(cell);
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUint32Le(bits, out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<double>(cell)) {
    const double value = std::get<double>(cell);
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUint64Le(bits, out);
    return absl::OkStatus();
  }
  if (std::holds_alternative<std::string>(cell)) {
    const std::string& value = std::get<std::string>(cell);
    const std::vector<uint8_t> length = encoding::EncodeVarint(value.size());
    if (!IsMinimalVarintEncoding(length)) {
      return absl::InvalidArgumentError("NON_MINIMAL_VARINT");
    }
    out->insert(out->end(), length.begin(), length.end());
    out->insert(out->end(), value.begin(), value.end());
    return absl::OkStatus();
  }
  const std::string& value = std::get<BytesValue>(cell).value;
  const std::vector<uint8_t> length = encoding::EncodeVarint(value.size());
  if (!IsMinimalVarintEncoding(length)) {
    return absl::InvalidArgumentError("NON_MINIMAL_VARINT");
  }
  out->insert(out->end(), length.begin(), length.end());
  out->insert(out->end(), value.begin(), value.end());
  return absl::OkStatus();
}

absl::StatusOr<std::string> EncodedIndexCellString(const IndexCell& cell) {
  std::vector<uint8_t> bytes;
  absl::Status encode_status = EncodeIndexCell(cell, &bytes);
  if (!encode_status.ok()) {
    return encode_status;
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct ResolvedKeyField {
  std::string field_path;
  std::vector<const google::protobuf::FieldDescriptor*> resolved_path;
};

absl::StatusOr<std::vector<IndexCell>> ExtractKeyCandidates(const google::protobuf::Message& artifact_message, const ResolvedKeyField& key_field) {
  auto parent_or = ResolveLeafParentMessage(artifact_message, key_field.resolved_path);
  if (!parent_or.ok()) {
    return parent_or.status();
  }
  const google::protobuf::Message* parent_message = *parent_or;
  if (parent_message == nullptr) {
    return std::vector<IndexCell>{};
  }

  const auto* leaf = key_field.resolved_path.back();
  const auto* reflection = parent_message->GetReflection();

  if (leaf->is_repeated()) {
    const int count = reflection->FieldSize(*parent_message, leaf);
    std::vector<IndexCell> values;
    std::set<std::string> seen;
    values.reserve(count);
    for (int i = 0; i < count; ++i) {
      auto value_or = ScalarValueForField(*parent_message, *leaf, i);
      if (!value_or.ok()) {
        return value_or.status();
      }
      auto encoded_or = EncodedIndexCellString(*value_or);
      if (!encoded_or.ok()) {
        return encoded_or.status();
      }
      if (seen.insert(*encoded_or).second) {
        values.push_back(*std::move(value_or));
      }
    }
    return values;
  }

  if (leaf->has_presence() && !reflection->HasField(*parent_message, leaf)) {
    return std::vector<IndexCell>{};
  }

  auto value_or = ScalarValueForField(*parent_message, *leaf, -1);
  if (!value_or.ok()) {
    return value_or.status();
  }
  return std::vector<IndexCell>{*std::move(value_or)};
}

struct KeySelection {
  std::vector<IndexCell> ordered_values;
  std::unordered_map<std::string, IndexCell> by_path;
};

void BuildKeyCombinations(const std::vector<ResolvedKeyField>& key_fields, const std::vector<std::vector<IndexCell>>& candidates_by_field, size_t cursor,
                          KeySelection* current, std::vector<KeySelection>* out) {
  if (cursor == candidates_by_field.size()) {
    out->push_back(*current);
    return;
  }
  for (const IndexCell& candidate : candidates_by_field[cursor]) {
    current->ordered_values.push_back(candidate);
    current->by_path[key_fields[cursor].field_path] = candidate;
    BuildKeyCombinations(key_fields, candidates_by_field, cursor + 1, current, out);
    current->ordered_values.pop_back();
    current->by_path.erase(key_fields[cursor].field_path);
  }
}

absl::StatusOr<std::optional<std::vector<IndexCell>>> ExtractOrderValues(const artifact_system::IndexDefinition& index_definition,
                                                                         const google::protobuf::Message& artifact_message, uint64_t artifact_id,
                                                                         const std::unordered_map<std::string, IndexCell>& key_selection_by_path) {
  std::vector<IndexCell> values;
  values.reserve(index_definition.order_size());

  for (const auto& order : index_definition.order()) {
    if (order.direction() == artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED) {
      return absl::InvalidArgumentError("index definition must include explicit order direction");
    }
    if (order.field() == "artifact_id") {
      values.push_back(artifact_id);
      continue;
    }

    auto path_or = ResolveFieldPath(*artifact_message.GetDescriptor(), order.field());
    if (!path_or.ok()) {
      return path_or.status();
    }
    auto parent_or = ResolveLeafParentMessage(artifact_message, *path_or);
    if (!parent_or.ok()) {
      return parent_or.status();
    }
    const google::protobuf::Message* parent = *parent_or;
    if (parent == nullptr) {
      return std::optional<std::vector<IndexCell>>{};
    }

    const auto* leaf = path_or->back();
    if (leaf->is_repeated()) {
      const auto selection_it = key_selection_by_path.find(order.field());
      if (selection_it == key_selection_by_path.end()) {
        return absl::InvalidArgumentError(absl::StrCat("repeated order field must also be a key field: ", order.field()));
      }
      values.push_back(selection_it->second);
      continue;
    }

    const auto* reflection = parent->GetReflection();
    if (leaf->has_presence() && !reflection->HasField(*parent, leaf)) {
      return std::optional<std::vector<IndexCell>>{};
    }
    auto value_or = ScalarValueForField(*parent, *leaf, -1);
    if (!value_or.ok()) {
      return value_or.status();
    }
    values.push_back(*std::move(value_or));
  }

  return values;
}

} // namespace

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntries(const google::protobuf::Descriptor& descriptor,
                                                                  const google::protobuf::Message& artifact_message, uint64_t artifact_id,
                                                                  const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  if (artifact_message.GetDescriptor() != &descriptor) {
    return absl::InvalidArgumentError("artifact message descriptor mismatch");
  }

  const auto& options = descriptor.options();
  std::vector<DerivedIndexEntry> out;

  for (int index_num = 0; index_num < options.ExtensionSize(artifact_system::indexes); ++index_num) {
    const artifact_system::IndexDefinition& index_definition = options.GetExtension(artifact_system::indexes, index_num);

    std::vector<ResolvedKeyField> key_fields;
    key_fields.reserve(index_definition.key_size());
    std::set<std::string> repeated_paths;
    for (const std::string& key_field : index_definition.key()) {
      auto resolved_or = ResolveFieldPath(descriptor, key_field);
      if (!resolved_or.ok()) {
        return resolved_or.status();
      }
      if (resolved_or->back()->is_repeated()) {
        repeated_paths.insert(key_field);
      }
      key_fields.push_back({key_field, *std::move(resolved_or)});
    }
    for (const auto& order : index_definition.order()) {
      if (order.field() == "artifact_id") {
        continue;
      }
      auto resolved_or = ResolveFieldPath(descriptor, order.field());
      if (!resolved_or.ok()) {
        return resolved_or.status();
      }
      if (resolved_or->back()->is_repeated()) {
        repeated_paths.insert(order.field());
      }
    }
    if (repeated_paths.size() > 1) {
      return absl::InvalidArgumentError("INVALID_INDEX_DEFINITION: at most one repeated field is allowed per index");
    }

    std::vector<std::vector<IndexCell>> candidates_by_field;
    candidates_by_field.reserve(key_fields.size());
    bool skip_index = false;
    for (const ResolvedKeyField& key_field : key_fields) {
      auto candidates_or = ExtractKeyCandidates(artifact_message, key_field);
      if (!candidates_or.ok()) {
        return candidates_or.status();
      }
      if (candidates_or->empty()) {
        skip_index = true;
        break;
      }
      candidates_by_field.push_back(*std::move(candidates_or));
    }
    if (skip_index) {
      continue;
    }

    std::vector<KeySelection> key_combinations;
    KeySelection current;
    BuildKeyCombinations(key_fields, candidates_by_field, 0, &current, &key_combinations);

    for (const KeySelection& key_selection : key_combinations) {
      auto order_values_or = ExtractOrderValues(index_definition, artifact_message, artifact_id, key_selection.by_path);
      if (!order_values_or.ok()) {
        return order_values_or.status();
      }
      if (!order_values_or->has_value()) {
        continue;
      }

      std::vector<uint8_t> encoded_key;
      for (const IndexCell& value : key_selection.ordered_values) {
        absl::Status encode_status = EncodeIndexCell(value, &encoded_key);
        if (!encode_status.ok()) {
          return encode_status;
        }
      }

      DerivedIndexEntry entry;
      const auto id_it = index_def_ids_by_key_type.find(index_definition.key_type());
      if (id_it != index_def_ids_by_key_type.end()) {
        entry.index_def_id = id_it->second;
      }
      entry.key_type = index_definition.key_type();
      entry.encoded_key = std::move(encoded_key);
      entry.order_values = std::move(order_values_or->value());
      entry.key_values = key_selection.ordered_values;
      out.push_back(std::move(entry));
    }
  }

  return out;
}

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntries(const google::protobuf::Descriptor& descriptor,
                                                                  const google::protobuf::Message& artifact_message, uint64_t artifact_id) {
  static const std::unordered_map<std::string, uint64_t> kEmpty;
  return DeriveIndexEntries(descriptor, artifact_message, artifact_id, kEmpty);
}

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntriesFromPayload(const google::protobuf::FileDescriptorSet& descriptor_set,
                                                                             const std::string& message_full_name, const std::string& payload,
                                                                             uint64_t artifact_id,
                                                                             const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  std::vector<bool> built(descriptor_set.file_size(), false);
  int built_count = 0;
  bool made_progress = true;
  while (built_count < descriptor_set.file_size() && made_progress) {
    made_progress = false;
    for (int i = 0; i < descriptor_set.file_size(); ++i) {
      if (built[static_cast<size_t>(i)]) {
        continue;
      }
      const auto& file = descriptor_set.file(i);
      if (pool.FindFileByName(file.name()) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
        continue;
      }
      if (pool.BuildFile(file) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
      }
    }
  }
  if (built_count < descriptor_set.file_size()) {
    for (int i = 0; i < descriptor_set.file_size(); ++i) {
      if (!built[static_cast<size_t>(i)]) {
        return absl::InvalidArgumentError(absl::StrCat("failed to build descriptor file: ", descriptor_set.file(i).name()));
      }
    }
  }

  const auto* descriptor = pool.FindMessageTypeByName(message_full_name);
  if (descriptor == nullptr) {
    return absl::InvalidArgumentError(absl::StrCat("message not found in descriptor set: ", message_full_name));
  }

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* prototype = factory.GetPrototype(descriptor);
  if (prototype == nullptr) {
    return absl::InternalError("failed to construct dynamic message prototype");
  }
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  if (!message->ParseFromString(payload)) {
    return absl::InvalidArgumentError("failed to parse artifact payload bytes");
  }

  return DeriveIndexEntries(*descriptor, *message, artifact_id, index_def_ids_by_key_type);
}

} // namespace artifact_system::index

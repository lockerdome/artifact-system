#include "index/index_object.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"

namespace artifact_system::index {
namespace {

absl::StatusOr<std::string> SerializeDeterministic(const google::protobuf::Message& message) {
  std::string out;
  const size_t byte_size = static_cast<size_t>(message.ByteSizeLong());
  if (byte_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::InvalidArgumentError("message too large for deterministic serialization");
  }
  out.resize(byte_size);
  google::protobuf::io::ArrayOutputStream raw_output(out.data(), static_cast<int>(byte_size));
  google::protobuf::io::CodedOutputStream coded_output(&raw_output);
  coded_output.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded_output)) {
    return absl::InternalError("failed to serialize message");
  }
  out.resize(coded_output.ByteCount());
  return out;
}

absl::StatusOr<IndexCell> ReadCell(const google::protobuf::Reflection& reflection, const google::protobuf::Message& message,
                                   const google::protobuf::FieldDescriptor& field, int index) {
  using Field = google::protobuf::FieldDescriptor;
  switch (field.type()) {
  case Field::TYPE_INT32:
  case Field::TYPE_SINT32:
  case Field::TYPE_SFIXED32:
    return reflection.GetRepeatedInt32(message, &field, index);
  case Field::TYPE_ENUM:
    return static_cast<int32_t>(reflection.GetRepeatedEnumValue(message, &field, index));
  case Field::TYPE_UINT32:
  case Field::TYPE_FIXED32:
    return reflection.GetRepeatedUInt32(message, &field, index);
  case Field::TYPE_INT64:
  case Field::TYPE_SINT64:
  case Field::TYPE_SFIXED64:
    return reflection.GetRepeatedInt64(message, &field, index);
  case Field::TYPE_UINT64:
  case Field::TYPE_FIXED64:
    return reflection.GetRepeatedUInt64(message, &field, index);
  case Field::TYPE_BOOL:
    return reflection.GetRepeatedBool(message, &field, index);
  case Field::TYPE_FLOAT:
    if (const float value = reflection.GetRepeatedFloat(message, &field, index); std::isnan(value)) {
      return absl::InvalidArgumentError("cannot deserialize NaN index cell values");
    } else {
      return value;
    }
  case Field::TYPE_DOUBLE:
    if (const double value = reflection.GetRepeatedDouble(message, &field, index); std::isnan(value)) {
      return absl::InvalidArgumentError("cannot deserialize NaN index cell values");
    } else {
      return value;
    }
  case Field::TYPE_STRING:
    return reflection.GetRepeatedString(message, &field, index);
  case Field::TYPE_BYTES:
    return BytesValue{reflection.GetRepeatedString(message, &field, index)};
  default:
    return absl::InvalidArgumentError(absl::StrCat("unsupported order field type for field: ", field.full_name()));
  }
}

absl::Status WriteCell(const IndexCell& cell, const google::protobuf::Reflection& reflection, google::protobuf::Message* message,
                       const google::protobuf::FieldDescriptor& field) {
  using Field = google::protobuf::FieldDescriptor;
  switch (field.type()) {
  case Field::TYPE_ENUM:
    if (!std::holds_alternative<int32_t>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected enum/int32)");
    }
    reflection.AddEnumValue(message, &field, std::get<int32_t>(cell));
    return absl::OkStatus();
  case Field::TYPE_INT32:
  case Field::TYPE_SINT32:
  case Field::TYPE_SFIXED32:
    if (!std::holds_alternative<int32_t>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected int32)");
    }
    reflection.AddInt32(message, &field, std::get<int32_t>(cell));
    return absl::OkStatus();
  case Field::TYPE_UINT32:
  case Field::TYPE_FIXED32:
    if (!std::holds_alternative<uint32_t>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected uint32)");
    }
    reflection.AddUInt32(message, &field, std::get<uint32_t>(cell));
    return absl::OkStatus();
  case Field::TYPE_INT64:
  case Field::TYPE_SINT64:
  case Field::TYPE_SFIXED64:
    if (!std::holds_alternative<int64_t>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected int64)");
    }
    reflection.AddInt64(message, &field, std::get<int64_t>(cell));
    return absl::OkStatus();
  case Field::TYPE_UINT64:
  case Field::TYPE_FIXED64:
    if (!std::holds_alternative<uint64_t>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected uint64)");
    }
    reflection.AddUInt64(message, &field, std::get<uint64_t>(cell));
    return absl::OkStatus();
  case Field::TYPE_BOOL:
    if (!std::holds_alternative<bool>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected bool)");
    }
    reflection.AddBool(message, &field, std::get<bool>(cell));
    return absl::OkStatus();
  case Field::TYPE_FLOAT:
    if (!std::holds_alternative<float>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected float)");
    }
    if (std::isnan(std::get<float>(cell))) {
      return absl::InvalidArgumentError("cannot serialize NaN index cell values");
    }
    reflection.AddFloat(message, &field, std::get<float>(cell));
    return absl::OkStatus();
  case Field::TYPE_DOUBLE:
    if (!std::holds_alternative<double>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected double)");
    }
    if (std::isnan(std::get<double>(cell))) {
      return absl::InvalidArgumentError("cannot serialize NaN index cell values");
    }
    reflection.AddDouble(message, &field, std::get<double>(cell));
    return absl::OkStatus();
  case Field::TYPE_STRING:
    if (!std::holds_alternative<std::string>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected string)");
    }
    reflection.AddString(message, &field, std::get<std::string>(cell));
    return absl::OkStatus();
  case Field::TYPE_BYTES:
    if (!std::holds_alternative<BytesValue>(cell)) {
      return absl::InvalidArgumentError("order value type mismatch (expected bytes)");
    }
    reflection.AddString(message, &field, std::get<BytesValue>(cell).value);
    return absl::OkStatus();
  default:
    return absl::InvalidArgumentError("unsupported order field type");
  }
}

absl::Status ValidateCompleteKey(const google::protobuf::Message& key_message, const artifact_system::IndexDefinition& index_definition) {
  const google::protobuf::Descriptor* key_descriptor = key_message.GetDescriptor();
  const google::protobuf::Reflection* key_reflection = key_message.GetReflection();
  for (int i = 0; i < index_definition.key_size(); ++i) {
    const auto* key_field = key_descriptor->FindFieldByNumber(i + 1);
    if (key_field == nullptr) {
      return absl::InternalError("generated key schema missing key field");
    }
    if (!key_reflection->HasField(key_message, key_field)) {
      return absl::InvalidArgumentError("serialized key is missing one or more required key fields");
    }
  }
  return absl::OkStatus();
}

} // namespace

absl::StatusOr<int> CompareIndexCellAscending(const IndexCell& lhs, const IndexCell& rhs) {
  if (lhs.index() != rhs.index()) {
    return absl::InvalidArgumentError("cannot compare index cells with different types");
  }

  if (std::holds_alternative<int32_t>(lhs)) {
    const int32_t a = std::get<int32_t>(lhs);
    const int32_t b = std::get<int32_t>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<uint32_t>(lhs)) {
    const uint32_t a = std::get<uint32_t>(lhs);
    const uint32_t b = std::get<uint32_t>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<int64_t>(lhs)) {
    const int64_t a = std::get<int64_t>(lhs);
    const int64_t b = std::get<int64_t>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<uint64_t>(lhs)) {
    const uint64_t a = std::get<uint64_t>(lhs);
    const uint64_t b = std::get<uint64_t>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<bool>(lhs)) {
    const bool a = std::get<bool>(lhs);
    const bool b = std::get<bool>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<float>(lhs)) {
    const float a = std::get<float>(lhs);
    const float b = std::get<float>(rhs);
    if (std::isnan(a) || std::isnan(b)) {
      return absl::InvalidArgumentError("cannot compare NaN index cell values");
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<double>(lhs)) {
    const double a = std::get<double>(lhs);
    const double b = std::get<double>(rhs);
    if (std::isnan(a) || std::isnan(b)) {
      return absl::InvalidArgumentError("cannot compare NaN index cell values");
    }
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }
  if (std::holds_alternative<std::string>(lhs)) {
    const std::string& a = std::get<std::string>(lhs);
    const std::string& b = std::get<std::string>(rhs);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  }

  const std::string& a = std::get<BytesValue>(lhs).value;
  const std::string& b = std::get<BytesValue>(rhs).value;
  return (a < b) ? -1 : (a > b ? 1 : 0);
}

absl::StatusOr<std::string> SerializeIndexObject(const GeneratedIndexSchema& schema, const artifact_system::IndexDefinition& index_definition,
                                                 const IndexObject& object) {
  if (schema.index_descriptor == nullptr || schema.key_descriptor == nullptr || schema.value_descriptor == nullptr) {
    return absl::InvalidArgumentError("schema is not initialized");
  }
  int artifact_id_order_count = 0;
  int artifact_id_order_index = -1;
  for (int i = 0; i < index_definition.order_size(); ++i) {
    const auto& order = index_definition.order(i);
    if (order.direction() == artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED) {
      return absl::InvalidArgumentError("index definition must include explicit order direction");
    }
    if (order.field() == "artifact_id") {
      ++artifact_id_order_count;
      artifact_id_order_index = i;
    }
  }
  if (artifact_id_order_count != 1 || artifact_id_order_index < 0) {
    return absl::InvalidArgumentError("index definition must include artifact_id exactly once in order fields");
  }

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* index_prototype = factory.GetPrototype(schema.index_descriptor);
  if (index_prototype == nullptr) {
    return absl::InternalError("failed to construct dynamic prototype for index schema");
  }
  std::unique_ptr<google::protobuf::Message> index_message(index_prototype->New());
  const google::protobuf::Reflection* index_reflection = index_message->GetReflection();

  const auto* key_field = schema.index_descriptor->FindFieldByName("key");
  const auto* value_field = schema.index_descriptor->FindFieldByName("value");
  if (key_field == nullptr || value_field == nullptr) {
    return absl::InternalError("generated index schema missing key/value fields");
  }

  google::protobuf::Message* key_message = index_reflection->MutableMessage(index_message.get(), key_field);
  if (index_definition.key_size() > 0 && object.serialized_key.empty()) {
    return absl::InvalidArgumentError("serialized key is required for indexes with non-empty key");
  }
  if (!object.serialized_key.empty() && !key_message->ParseFromString(object.serialized_key)) {
    return absl::InvalidArgumentError("failed to parse serialized index key bytes");
  }
  if (object.serialized_key.empty()) {
    key_message->Clear();
  } else {
    absl::Status key_status = ValidateCompleteKey(*key_message, index_definition);
    if (!key_status.ok()) {
      return key_status;
    }
  }

  google::protobuf::Message* value_message = index_reflection->MutableMessage(index_message.get(), value_field);
  const google::protobuf::Reflection* value_reflection = value_message->GetReflection();
  const auto* row_count_field = schema.value_descriptor->FindFieldByName("row_count");
  if (row_count_field == nullptr) {
    return absl::InternalError("generated index value schema missing row_count");
  }
  if (object.rows.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    return absl::InvalidArgumentError("row_count exceeds uint32 max");
  }
  if (object.rows.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return absl::InvalidArgumentError("row_count exceeds protobuf repeated-field index range");
  }
  std::vector<IndexRow> rows = object.rows;
  std::unordered_set<uint64_t> seen_artifact_ids;
  seen_artifact_ids.reserve(rows.size());
  for (const IndexRow& row : rows) {
    if (!seen_artifact_ids.insert(row.artifact_id).second) {
      return absl::InvalidArgumentError("duplicate artifact_id rows are not allowed");
    }
    if (row.order_values.size() != static_cast<size_t>(index_definition.order_size())) {
      return absl::InvalidArgumentError("row order value count does not match index definition");
    }
    if (artifact_id_order_index < 0 || static_cast<size_t>(artifact_id_order_index) >= row.order_values.size()) {
      return absl::InvalidArgumentError("artifact_id order field value missing from row");
    }
    const IndexCell& artifact_id_cell = row.order_values[static_cast<size_t>(artifact_id_order_index)];
    if (!std::holds_alternative<uint64_t>(artifact_id_cell)) {
      return absl::InvalidArgumentError("artifact_id order field value must be uint64");
    }
    if (std::get<uint64_t>(artifact_id_cell) != row.artifact_id) {
      return absl::InvalidArgumentError("row artifact_id does not match artifact_id order field value");
    }
  }

  auto compare_rows_or = [&](const IndexRow& lhs, const IndexRow& rhs) -> absl::StatusOr<int> {
    for (int i = 0; i < index_definition.order_size(); ++i) {
      auto cmp_or = CompareIndexCellAscending(lhs.order_values[i], rhs.order_values[i]);
      if (!cmp_or.ok()) {
        return cmp_or.status();
      }
      if (*cmp_or == 0) {
        continue;
      }
      if (index_definition.order(i).direction() == artifact_system::OrderDefinition::DESCENDING) {
        return -*cmp_or;
      }
      return *cmp_or;
    }
    return 0;
  };

  for (size_t i = 0; i < rows.size(); ++i) {
    for (size_t j = i + 1; j < rows.size(); ++j) {
      auto cmp_or = compare_rows_or(rows[i], rows[j]);
      if (!cmp_or.ok()) {
        return cmp_or.status();
      }
    }
  }

  std::sort(rows.begin(), rows.end(), [&](const IndexRow& lhs, const IndexRow& rhs) {
    const auto cmp_or = compare_rows_or(lhs, rhs);
    if (*cmp_or != 0) {
      return *cmp_or < 0;
    }
    return lhs.artifact_id < rhs.artifact_id;
  });

  value_message->Clear();
  value_reflection->SetUInt32(value_message, row_count_field, static_cast<uint32_t>(rows.size()));
  for (const IndexRow& row : rows) {
    for (int i = 0; i < index_definition.order_size(); ++i) {
      const auto* field = schema.value_descriptor->FindFieldByNumber(i + 2);
      if (field == nullptr) {
        return absl::InternalError("generated value schema missing order field");
      }
      absl::Status write_status = WriteCell(row.order_values[i], *value_reflection, value_message, *field);
      if (!write_status.ok()) {
        return write_status;
      }
    }
  }

  return SerializeDeterministic(*index_message);
}

absl::StatusOr<IndexObject> DeserializeIndexObject(const GeneratedIndexSchema& schema, const artifact_system::IndexDefinition& index_definition,
                                                   const std::string& bytes) {
  if (schema.index_descriptor == nullptr || schema.key_descriptor == nullptr || schema.value_descriptor == nullptr) {
    return absl::InvalidArgumentError("schema is not initialized");
  }

  int artifact_id_order_count = 0;
  for (const auto& order : index_definition.order()) {
    if (order.direction() == artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED) {
      return absl::InvalidArgumentError("index definition must include explicit order direction");
    }
    if (order.field() == "artifact_id") {
      ++artifact_id_order_count;
    }
  }
  if (artifact_id_order_count != 1) {
    return absl::InvalidArgumentError("index definition must include artifact_id exactly once in order fields");
  }

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* prototype = factory.GetPrototype(schema.index_descriptor);
  if (prototype == nullptr) {
    return absl::InternalError("failed to construct dynamic prototype for index schema");
  }
  std::unique_ptr<google::protobuf::Message> index_message(prototype->New());
  if (!index_message->ParseFromString(bytes)) {
    return absl::InvalidArgumentError("failed to parse index object bytes");
  }

  const google::protobuf::Reflection* index_reflection = index_message->GetReflection();
  const auto* key_field = schema.index_descriptor->FindFieldByName("key");
  const auto* value_field = schema.index_descriptor->FindFieldByName("value");
  if (key_field == nullptr || value_field == nullptr) {
    return absl::InternalError("generated index schema missing key/value fields");
  }

  IndexObject out;
  if (index_reflection->HasField(*index_message, key_field)) {
    auto serialized_key_or = SerializeDeterministic(index_reflection->GetMessage(*index_message, key_field));
    if (!serialized_key_or.ok()) {
      return serialized_key_or.status();
    }
    out.serialized_key = *serialized_key_or;
    absl::Status key_status = ValidateCompleteKey(index_reflection->GetMessage(*index_message, key_field), index_definition);
    if (!key_status.ok()) {
      return key_status;
    }
  }
  if (index_definition.key_size() > 0 && out.serialized_key.empty()) {
    return absl::InvalidArgumentError("serialized key missing for index with non-empty key");
  }

  if (!index_reflection->HasField(*index_message, value_field)) {
    return out;
  }

  const google::protobuf::Message& value_message = index_reflection->GetMessage(*index_message, value_field);
  const google::protobuf::Reflection* value_reflection = value_message.GetReflection();
  const auto* row_count_field = schema.value_descriptor->FindFieldByName("row_count");
  if (row_count_field == nullptr) {
    return absl::InternalError("generated index value schema missing row_count");
  }
  const uint64_t row_count = value_reflection->GetUInt32(value_message, row_count_field);
  if (row_count > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    return absl::InvalidArgumentError("row_count exceeds protobuf repeated-field index range");
  }

  for (int i = 0; i < index_definition.order_size(); ++i) {
    const auto* field = schema.value_descriptor->FindFieldByNumber(i + 2);
    if (field == nullptr) {
      return absl::InternalError("generated value schema missing order field");
    }
    if (static_cast<uint64_t>(value_reflection->FieldSize(value_message, field)) != row_count) {
      return absl::InvalidArgumentError(absl::StrCat("row_count mismatch on field: ", field->name()));
    }
  }

  out.rows.reserve(row_count);
  std::unordered_set<uint64_t> seen_artifact_ids;
  seen_artifact_ids.reserve(static_cast<size_t>(row_count));
  for (uint64_t row_index = 0; row_index < row_count; ++row_index) {
    IndexRow row;
    bool row_has_artifact_id = false;
    row.order_values.reserve(index_definition.order_size());
    for (int i = 0; i < index_definition.order_size(); ++i) {
      const auto* field = schema.value_descriptor->FindFieldByNumber(i + 2);
      auto cell_or = ReadCell(*value_reflection, value_message, *field, static_cast<int>(row_index));
      if (!cell_or.ok()) {
        return cell_or.status();
      }
      row.order_values.push_back(*std::move(cell_or));
      if (index_definition.order(i).field() == "artifact_id") {
        if (!std::holds_alternative<uint64_t>(row.order_values.back())) {
          return absl::InternalError("artifact_id order field is not uint64 in generated schema");
        }
        row.artifact_id = std::get<uint64_t>(row.order_values.back());
        row_has_artifact_id = true;
      }
    }
    if (!row_has_artifact_id) {
      return absl::InvalidArgumentError("artifact_id order field value missing from row");
    }
    if (!seen_artifact_ids.insert(row.artifact_id).second) {
      return absl::InvalidArgumentError("duplicate artifact_id rows are not allowed");
    }
    out.rows.push_back(std::move(row));
  }

  return out;
}

} // namespace artifact_system::index

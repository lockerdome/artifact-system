#include "encoding/index_key_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"

namespace artifact_system::encoding {

namespace {

void AppendUint32LittleEndian(uint32_t value, std::vector<uint8_t>* out) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

void AppendUint64LittleEndian(uint64_t value, std::vector<uint8_t>* out) {
  out->push_back(static_cast<uint8_t>(value & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 8U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 32U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 40U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 48U) & 0xFFU));
  out->push_back(static_cast<uint8_t>((value >> 56U) & 0xFFU));
}

absl::StatusOr<std::vector<const google::protobuf::FieldDescriptor*>> ResolveFieldPath(const google::protobuf::Descriptor& descriptor,
                                                                                       std::string_view field_path) {
  if (field_path.empty()) {
    return absl::InvalidArgumentError("key field path cannot be empty");
  }

  std::vector<const google::protobuf::FieldDescriptor*> resolved;
  const google::protobuf::Descriptor* current = &descriptor;

  for (const std::string_view segment : absl::StrSplit(field_path, '.')) {
    if (segment.empty()) {
      return absl::InvalidArgumentError(absl::StrCat("invalid field path segment in: ", field_path));
    }

    const auto* field = current->FindFieldByName(segment);
    if (field == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat("field not found in path '", field_path, "': ", segment));
    }

    if (field->is_map() || field->is_repeated()) {
      return absl::InvalidArgumentError(absl::StrCat("map/repeated fields are not supported by concrete EncodeKey tuples: ", field_path));
    }

    resolved.push_back(field);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      current = field->message_type();
    }
  }

  if (resolved.back()->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return absl::InvalidArgumentError(absl::StrCat("final key field must be scalar/enum, got message in path: ", field_path));
  }

  return resolved;
}

absl::Status AppendLengthPrefixedBytes(std::span<const uint8_t> bytes, std::vector<uint8_t>* out) {
  const auto length = EncodeVarint(bytes.size());
  out->insert(out->end(), length.begin(), length.end());
  out->insert(out->end(), bytes.begin(), bytes.end());
  return absl::OkStatus();
}

absl::Status AppendFieldValue(const google::protobuf::Message& message, const google::protobuf::FieldDescriptor& field, std::vector<uint8_t>* out) {
  const google::protobuf::Reflection* reflection = message.GetReflection();

  using Field = google::protobuf::FieldDescriptor;
  switch (field.type()) {
  case Field::TYPE_INT32:
  case Field::TYPE_SINT32:
  case Field::TYPE_SFIXED32: {
    const int32_t value = reflection->GetInt32(message, &field);
    AppendUint32LittleEndian(static_cast<uint32_t>(value), out);
    return absl::OkStatus();
  }
  case Field::TYPE_UINT32:
  case Field::TYPE_FIXED32: {
    const uint32_t value = reflection->GetUInt32(message, &field);
    AppendUint32LittleEndian(value, out);
    return absl::OkStatus();
  }
  case Field::TYPE_INT64:
  case Field::TYPE_SINT64:
  case Field::TYPE_SFIXED64: {
    const int64_t value = reflection->GetInt64(message, &field);
    AppendUint64LittleEndian(static_cast<uint64_t>(value), out);
    return absl::OkStatus();
  }
  case Field::TYPE_UINT64:
  case Field::TYPE_FIXED64: {
    const uint64_t value = reflection->GetUInt64(message, &field);
    AppendUint64LittleEndian(value, out);
    return absl::OkStatus();
  }
  case Field::TYPE_BOOL: {
    out->push_back(reflection->GetBool(message, &field) ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0));
    return absl::OkStatus();
  }
  case Field::TYPE_ENUM: {
    const int enum_value = reflection->GetEnumValue(message, &field);
    if (enum_value < std::numeric_limits<int32_t>::min() || enum_value > std::numeric_limits<int32_t>::max()) {
      return absl::InvalidArgumentError(absl::StrCat("enum value out of int32 range for field: ", field.full_name()));
    }
    AppendUint32LittleEndian(static_cast<uint32_t>(static_cast<int32_t>(enum_value)), out);
    return absl::OkStatus();
  }
  case Field::TYPE_FLOAT: {
    float value = reflection->GetFloat(message, &field);
    if (std::isnan(value)) {
      return absl::InvalidArgumentError(absl::StrCat("NAN_IN_INDEXED_FIELD: ", field.full_name()));
    }
    if (value == 0.0F) {
      value = 0.0F;
    }
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUint32LittleEndian(bits, out);
    return absl::OkStatus();
  }
  case Field::TYPE_DOUBLE: {
    double value = reflection->GetDouble(message, &field);
    if (std::isnan(value)) {
      return absl::InvalidArgumentError(absl::StrCat("NAN_IN_INDEXED_FIELD: ", field.full_name()));
    }
    if (value == 0.0) {
      value = 0.0;
    }
    static_assert(sizeof(double) == sizeof(uint64_t));
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUint64LittleEndian(bits, out);
    return absl::OkStatus();
  }
  case Field::TYPE_STRING: {
    const std::string value = reflection->GetString(message, &field);
    return AppendLengthPrefixedBytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()), out);
  }
  case Field::TYPE_BYTES: {
    const std::string value = reflection->GetString(message, &field);
    return AppendLengthPrefixedBytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()), out);
  }
  default:
    return absl::InvalidArgumentError(absl::StrCat("unsupported key field type for field: ", field.full_name()));
  }
}

} // namespace

std::vector<uint8_t> EncodeVarint(uint64_t value) {
  std::vector<uint8_t> out;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (value != 0);
  return out;
}

absl::StatusOr<uint64_t> DecodeVarint(std::span<const uint8_t> bytes, size_t* bytes_read) {
  if (bytes_read == nullptr) {
    return absl::InvalidArgumentError("bytes_read must not be null");
  }

  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size() && i < 10; ++i) {
    const uint8_t byte = bytes[i];

    if (i < 9) {
      const uint64_t payload = static_cast<uint64_t>(byte & 0x7FU);
      value |= (payload << (i * 7U));
      if ((byte & 0x80U) == 0) {
        *bytes_read = i + 1;
        const std::vector<uint8_t> canonical = EncodeVarint(value);
        if (canonical.size() != *bytes_read || !std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.begin() + *bytes_read)) {
          return absl::InvalidArgumentError("NON_MINIMAL_VARINT");
        }
        return value;
      }
      continue;
    }

    if ((byte & 0x80U) != 0) {
      return absl::InvalidArgumentError("unterminated varint");
    }
    if (byte > 1U) {
      return absl::InvalidArgumentError("varint overflow");
    }

    value |= (static_cast<uint64_t>(byte) << 63U);
    *bytes_read = 10;
    const std::vector<uint8_t> canonical = EncodeVarint(value);
    if (canonical.size() != *bytes_read || !std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.begin() + *bytes_read)) {
      return absl::InvalidArgumentError("NON_MINIMAL_VARINT");
    }
    return value;
  }

  return absl::InvalidArgumentError("unterminated varint");
}

absl::StatusOr<std::vector<uint8_t>> EncodeKey(const google::protobuf::Descriptor& descriptor, const google::protobuf::Message& message,
                                               std::span<const std::string> key_fields) {
  if (message.GetDescriptor() != &descriptor) {
    return absl::InvalidArgumentError(absl::StrCat("descriptor mismatch: expected ", descriptor.full_name(), ", got ", message.GetDescriptor()->full_name()));
  }

  if (key_fields.empty()) {
    return absl::InvalidArgumentError("at least one key field is required");
  }

  std::vector<uint8_t> out;

  for (const std::string& field_path : key_fields) {
    auto resolved_path_or = ResolveFieldPath(descriptor, field_path);
    if (!resolved_path_or.ok()) {
      return resolved_path_or.status();
    }

    const auto& resolved_path = *resolved_path_or;
    const google::protobuf::Message* current_message = &message;

    for (size_t i = 0; i + 1 < resolved_path.size(); ++i) {
      const auto* field = resolved_path[i];
      const auto* reflection = current_message->GetReflection();
      if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        return absl::InvalidArgumentError(absl::StrCat("non-message segment in key field path: ", field_path));
      }
      if (field->has_presence() && !reflection->HasField(*current_message, field)) {
        return absl::InvalidArgumentError(absl::StrCat("missing intermediate message field in path: ", field_path));
      }
      current_message = &reflection->GetMessage(*current_message, field);
    }

    const auto* leaf = resolved_path.back();
    const auto* leaf_reflection = current_message->GetReflection();
    if (leaf->has_presence() && !leaf_reflection->HasField(*current_message, leaf)) {
      return absl::InvalidArgumentError(absl::StrCat("missing key field for concrete EncodeKey tuple: ", field_path));
    }

    absl::Status append_status = AppendFieldValue(*current_message, *leaf, &out);
    if (!append_status.ok()) {
      return append_status;
    }
  }

  return out;
}

} // namespace artifact_system::encoding

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
#include "absl/strings/str_split.h"
#include "artifact/field_path.h"
#include "artifact_options.pb.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"
#include "index/repeated_chain_analysis.h"

namespace artifact_system::index {
namespace {

struct IndexFieldInfo {
  std::string path;
  artifact::ResolvedIndexFieldPath resolved;
};

struct ExpansionContext {
  std::unordered_map<std::string, int> repeated_indices;
};

// Navigate from root through the segments of a resolved field path to reach the
// parent message of the leaf field. Uses ctx.repeated_indices for repeated
// message intermediates. Returns nullptr if an optional message is not set.
absl::StatusOr<const google::protobuf::Message*> NavigateToLeafParent(const google::protobuf::Message& root,
                                                                      const artifact::ResolvedIndexFieldPath& resolved, std::string_view path,
                                                                      const ExpansionContext& ctx) {
  const google::protobuf::Message* current = &root;
  const std::vector<std::string_view> parts = absl::StrSplit(path, '.');
  std::string prefix;

  for (size_t i = 0; i + 1 < resolved.segments.size(); ++i) {
    const auto& seg = resolved.segments[i];
    if (seg.is_virtual_index) {
      break;
    }
    if (!prefix.empty()) {
      prefix.push_back('.');
    }
    prefix.append(parts[i].data(), parts[i].size());

    const auto* reflection = current->GetReflection();
    if (seg.is_repeated) {
      auto it = ctx.repeated_indices.find(prefix);
      if (it == ctx.repeated_indices.end()) {
        return absl::InternalError(absl::StrCat("repeated index missing for: ", prefix));
      }
      if (it->second >= reflection->FieldSize(*current, seg.descriptor)) {
        return static_cast<const google::protobuf::Message*>(nullptr);
      }
      current = &reflection->GetRepeatedMessage(*current, seg.descriptor, it->second);
    } else {
      if (seg.descriptor->has_presence() && !reflection->HasField(*current, seg.descriptor)) {
        return static_cast<const google::protobuf::Message*>(nullptr);
      }
      current = &reflection->GetMessage(*current, seg.descriptor);
    }
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

// Extract a scalar value for a field given the current expansion context.
// Returns nullopt if the value is absent (optional not set), meaning the entry
// should be skipped.
absl::StatusOr<std::optional<IndexCell>> ExtractFieldValueInContext(const google::protobuf::Message& root, const IndexFieldInfo& field_info,
                                                                    const ExpansionContext& ctx) {
  // Virtual _index field: return the iteration index of the parent repeated.
  if (field_info.resolved.leaf_is_virtual_index) {
    const size_t last_dot = field_info.path.rfind('.');
    if (last_dot == std::string::npos) {
      return absl::InternalError("_index path has no parent prefix");
    }
    const std::string parent_prefix = field_info.path.substr(0, last_dot);
    const auto it = ctx.repeated_indices.find(parent_prefix);
    if (it == ctx.repeated_indices.end()) {
      return absl::InternalError(absl::StrCat("_index context missing for: ", parent_prefix));
    }
    return std::optional<IndexCell>(IndexCell(static_cast<uint32_t>(it->second)));
  }

  auto parent_or = NavigateToLeafParent(root, field_info.resolved, field_info.path, ctx);
  if (!parent_or.ok()) {
    return parent_or.status();
  }
  if (*parent_or == nullptr) {
    return std::optional<IndexCell>{};
  }

  const auto* parent = *parent_or;
  const auto& leaf_seg = field_info.resolved.segments.back();
  const auto* leaf_fd = leaf_seg.descriptor;
  const auto* reflection = parent->GetReflection();

  if (leaf_seg.is_repeated) {
    const auto it = ctx.repeated_indices.find(field_info.path);
    if (it == ctx.repeated_indices.end()) {
      return absl::InternalError(absl::StrCat("repeated index missing for leaf: ", field_info.path));
    }
    if (it->second >= reflection->FieldSize(*parent, leaf_fd)) {
      return std::optional<IndexCell>{};
    }
    auto val = ScalarValueForField(*parent, *leaf_fd, it->second);
    if (!val.ok()) {
      return val.status();
    }
    return std::optional<IndexCell>(*std::move(val));
  }

  if (leaf_fd->has_presence() && !reflection->HasField(*parent, leaf_fd)) {
    return std::optional<IndexCell>{};
  }
  auto val = ScalarValueForField(*parent, *leaf_fd, -1);
  if (!val.ok()) {
    return val.status();
  }
  return std::optional<IndexCell>(*std::move(val));
}

// Get the element count for a repeated field at a given chain depth.
absl::StatusOr<int> GetChainFieldCount(const google::protobuf::Message& root, const RepeatedChainAnalysis& analysis, int chain_depth,
                                       const ExpansionContext& ctx) {
  const auto& ancestor = analysis.chain[static_cast<size_t>(chain_depth)];
  const std::vector<std::string_view> parts = absl::StrSplit(ancestor.path_prefix, '.');

  // Navigate to the parent message containing this repeated field.
  const google::protobuf::Message* parent = &root;
  std::string prefix;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!prefix.empty()) {
      prefix.push_back('.');
    }
    prefix.append(parts[i].data(), parts[i].size());

    const auto* field = parent->GetDescriptor()->FindFieldByName(std::string(parts[i]));
    if (field == nullptr) {
      return absl::InternalError(absl::StrCat("field not found: ", parts[i]));
    }
    const auto* reflection = parent->GetReflection();
    if (field->is_repeated()) {
      const auto it = ctx.repeated_indices.find(prefix);
      if (it == ctx.repeated_indices.end()) {
        return absl::InternalError(absl::StrCat("repeated index missing for: ", prefix));
      }
      if (it->second >= reflection->FieldSize(*parent, field)) {
        return 0;
      }
      parent = &reflection->GetRepeatedMessage(*parent, field, it->second);
    } else {
      if (field->has_presence() && !reflection->HasField(*parent, field)) {
        return 0;
      }
      parent = &reflection->GetMessage(*parent, field);
    }
  }

  return parent->GetReflection()->FieldSize(*parent, ancestor.descriptor);
}

// Emit a single index entry from the current expansion context.
absl::StatusOr<std::optional<DerivedIndexEntry>> EmitEntry(const google::protobuf::Message& root, const std::vector<IndexFieldInfo>& key_fields,
                                                           const artifact_system::IndexDefinition& index_def,
                                                           const std::vector<IndexFieldInfo>& order_fields, uint64_t artifact_id,
                                                           uint64_t index_def_id, const ExpansionContext& ctx) {
  std::vector<IndexCell> key_values;
  key_values.reserve(key_fields.size());
  for (const auto& kf : key_fields) {
    auto val_or = ExtractFieldValueInContext(root, kf, ctx);
    if (!val_or.ok()) {
      return val_or.status();
    }
    if (!val_or->has_value()) {
      return std::optional<DerivedIndexEntry>{};
    }
    key_values.push_back(std::move(val_or->value()));
  }

  std::vector<IndexCell> order_values;
  order_values.reserve(static_cast<size_t>(index_def.order_size()));
  for (const auto& of : order_fields) {
    if (of.path == "artifact_id") {
      order_values.push_back(artifact_id);
      continue;
    }
    auto val_or = ExtractFieldValueInContext(root, of, ctx);
    if (!val_or.ok()) {
      return val_or.status();
    }
    if (!val_or->has_value()) {
      return std::optional<DerivedIndexEntry>{};
    }
    order_values.push_back(std::move(val_or->value()));
  }

  std::vector<uint8_t> encoded_key;
  for (const IndexCell& value : key_values) {
    absl::Status encode_status = EncodeIndexCell(value, &encoded_key);
    if (!encode_status.ok()) {
      return encode_status;
    }
  }

  DerivedIndexEntry entry;
  entry.index_def_id = index_def_id;
  entry.key_type = index_def.key_type();
  entry.encoded_key = std::move(encoded_key);
  entry.order_values = std::move(order_values);
  entry.key_values = std::move(key_values);
  return std::optional<DerivedIndexEntry>(std::move(entry));
}

// Recursively expand nested repeated fields, emitting index entries at the
// deepest level. Each recursion level iterates over one repeated field in the
// ancestry chain.
absl::Status ExpandNestedRepeated(const google::protobuf::Message& root, const RepeatedChainAnalysis& analysis,
                                  const std::vector<IndexFieldInfo>& key_fields, const artifact_system::IndexDefinition& index_def,
                                  const std::vector<IndexFieldInfo>& order_fields, uint64_t artifact_id, uint64_t index_def_id, int chain_depth,
                                  ExpansionContext* ctx, std::vector<DerivedIndexEntry>* out) {
  if (chain_depth == static_cast<int>(analysis.chain.size())) {
    auto entry_or = EmitEntry(root, key_fields, index_def, order_fields, artifact_id, index_def_id, *ctx);
    if (!entry_or.ok()) {
      return entry_or.status();
    }
    if (entry_or->has_value()) {
      out->push_back(std::move(entry_or->value()));
    }
    return absl::OkStatus();
  }

  auto count_or = GetChainFieldCount(root, analysis, chain_depth, *ctx);
  if (!count_or.ok()) {
    return count_or.status();
  }
  const int count = *count_or;

  const auto& ancestor = analysis.chain[static_cast<size_t>(chain_depth)];
  for (int i = 0; i < count; ++i) {
    ctx->repeated_indices[ancestor.path_prefix] = i;
    absl::Status status = ExpandNestedRepeated(root, analysis, key_fields, index_def, order_fields, artifact_id, index_def_id, chain_depth + 1, ctx, out);
    if (!status.ok()) {
      return status;
    }
  }
  ctx->repeated_indices.erase(ancestor.path_prefix);
  return absl::OkStatus();
}

// Deduplicate entries with identical encoded key and order values.
void DeduplicateEntries(std::vector<DerivedIndexEntry>* entries) {
  std::set<std::string> seen;
  auto write = entries->begin();
  for (auto read = entries->begin(); read != entries->end(); ++read) {
    std::vector<uint8_t> fingerprint = read->encoded_key;
    for (const auto& cell : read->order_values) {
      // Errors in fingerprint encoding are not possible here since the cells
      // were already validated during extraction.
      EncodeIndexCell(cell, &fingerprint).IgnoreError();
    }
    std::string fp_str(reinterpret_cast<const char*>(fingerprint.data()), fingerprint.size());
    if (seen.insert(std::move(fp_str)).second) {
      if (write != read) {
        *write = std::move(*read);
      }
      ++write;
    }
  }
  entries->erase(write, entries->end());
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
    const artifact_system::IndexDefinition& index_def = options.GetExtension(artifact_system::indexes, index_num);

    // Resolve key fields.
    std::vector<IndexFieldInfo> key_fields;
    std::vector<std::string> key_paths;
    key_fields.reserve(index_def.key_size());
    key_paths.reserve(index_def.key_size());
    for (const auto& key_path : index_def.key()) {
      auto resolved_or = artifact::ResolveIndexFieldPath(descriptor, key_path);
      if (!resolved_or.ok()) {
        return resolved_or.status();
      }
      key_fields.push_back({key_path, *std::move(resolved_or)});
      key_paths.push_back(key_path);
    }

    // Resolve order fields. "artifact_id" gets a placeholder.
    std::vector<IndexFieldInfo> order_fields;
    std::vector<std::string> order_paths_for_analysis;
    order_fields.reserve(index_def.order_size());
    for (const auto& order : index_def.order()) {
      if (order.field() == "artifact_id") {
        IndexFieldInfo info;
        info.path = "artifact_id";
        order_fields.push_back(std::move(info));
        continue;
      }
      auto resolved_or = artifact::ResolveIndexFieldPath(descriptor, order.field());
      if (!resolved_or.ok()) {
        return resolved_or.status();
      }
      order_fields.push_back({order.field(), *std::move(resolved_or)});
      order_paths_for_analysis.push_back(order.field());
    }

    // Analyze repeated ancestry chain.
    auto analysis_or = AnalyzeRepeatedChain(descriptor, key_paths, order_paths_for_analysis);
    if (!analysis_or.ok()) {
      return analysis_or.status();
    }
    const auto& analysis = *analysis_or;

    uint64_t index_def_id = 0;
    const auto id_it = index_def_ids_by_key_type.find(index_def.key_type());
    if (id_it != index_def_ids_by_key_type.end()) {
      index_def_id = id_it->second;
    }

    if (analysis.chain.empty()) {
      // No repeated fields: emit a single entry directly.
      ExpansionContext ctx;
      auto entry_or = EmitEntry(artifact_message, key_fields, index_def, order_fields, artifact_id, index_def_id, ctx);
      if (!entry_or.ok()) {
        return entry_or.status();
      }
      if (entry_or->has_value()) {
        out.push_back(std::move(entry_or->value()));
      }
    } else {
      // Recursive expansion through nested repeated chain.
      ExpansionContext ctx;
      std::vector<DerivedIndexEntry> index_entries;
      absl::Status status =
          ExpandNestedRepeated(artifact_message, analysis, key_fields, index_def, order_fields, artifact_id, index_def_id, 0, &ctx, &index_entries);
      if (!status.ok()) {
        return status;
      }
      DeduplicateEntries(&index_entries);
      out.insert(out.end(), std::make_move_iterator(index_entries.begin()), std::make_move_iterator(index_entries.end()));
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

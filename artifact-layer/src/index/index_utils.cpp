#include "index/index_utils.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/dynamic_message.h"

#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"

namespace artifact_system::index {
namespace {

absl::Status IndexDefinitionNotFoundStatus(const std::string& key_type) {
  return absl::NotFoundError(absl::StrCat("index definition not found for key_type: ", key_type));
}

} // namespace

std::optional<IndexDefinition> FindIndexDefinition(const google::protobuf::Descriptor& descriptor, const std::string& key_type) {
  const auto& options = descriptor.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    const auto& def = options.GetExtension(artifact_system::indexes, i);
    if (def.key_type() == key_type) {
      return def;
    }
  }
  return std::nullopt;
}

absl::StatusOr<uint64_t> ResolveIndexDefinitionId(StorageInterface* storage, const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type,
                                                  const std::string& key_type) {
  auto id_it = index_def_ids_by_key_type.find(key_type);
  if (id_it != index_def_ids_by_key_type.end()) {
    return id_it->second;
  }

  auto lookup_index_it = index_def_ids_by_key_type.find("index_key_type_unique");
  if (lookup_index_it == index_def_ids_by_key_type.end()) {
    return absl::InternalError("index_key_type_unique index not in cache -- cannot resolve cache miss");
  }

  const auto* descriptor = IndexDefinition::descriptor();
  auto encoded_or = encoding::EncodeSingleStringKey(*descriptor, "key_type", key_type);
  if (!encoded_or.ok()) {
    return encoded_or.status();
  }

  const IndexDefinition& lookup_index_definition = descriptor->options().GetExtension(artifact_system::indexes, 0);
  auto schema_or = GenerateIndexSchema(lookup_index_definition, *descriptor);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  const std::string ref = storage->GetCanonicalBranch();
  const std::string index_path = encoding::IndexPath(lookup_index_it->second, *encoded_or);
  auto index_data_or = storage->GetObject(ref, index_path);
  if (!index_data_or.ok()) {
    if (absl::IsNotFound(index_data_or.status())) {
      return IndexDefinitionNotFoundStatus(key_type);
    }
    return index_data_or.status();
  }

  auto index_obj_or = DeserializeIndexObject(*schema_or, lookup_index_definition, *index_data_or);
  if (!index_obj_or.ok()) {
    return index_obj_or.status();
  }
  if (index_obj_or->rows.empty()) {
    return IndexDefinitionNotFoundStatus(key_type);
  }

  return index_obj_or->rows.front().artifact_id;
}

absl::StatusOr<std::string> BuildProtoSerializedKey(const GeneratedIndexSchema& schema, const std::vector<IndexCell>& key_values) {
  if (key_values.empty())
    return std::string{};

  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(schema.key_descriptor);
  std::unique_ptr<google::protobuf::Message> key_msg(prototype->New());
  const auto* reflection = key_msg->GetReflection();

  for (int i = 0; i < static_cast<int>(key_values.size()); ++i) {
    const auto* field = schema.key_descriptor->FindFieldByNumber(i + 1);
    if (field == nullptr)
      return absl::InternalError("generated key schema missing field");

    const auto& cell = key_values[static_cast<size_t>(i)];
    if (std::holds_alternative<std::string>(cell)) {
      reflection->SetString(key_msg.get(), field, std::get<std::string>(cell));
    } else if (std::holds_alternative<uint64_t>(cell)) {
      reflection->SetUInt64(key_msg.get(), field, std::get<uint64_t>(cell));
    } else if (std::holds_alternative<int64_t>(cell)) {
      reflection->SetInt64(key_msg.get(), field, std::get<int64_t>(cell));
    } else if (std::holds_alternative<uint32_t>(cell)) {
      reflection->SetUInt32(key_msg.get(), field, std::get<uint32_t>(cell));
    } else if (std::holds_alternative<int32_t>(cell)) {
      reflection->SetInt32(key_msg.get(), field, std::get<int32_t>(cell));
    } else if (std::holds_alternative<bool>(cell)) {
      reflection->SetBool(key_msg.get(), field, std::get<bool>(cell));
    } else if (std::holds_alternative<float>(cell)) {
      reflection->SetFloat(key_msg.get(), field, std::get<float>(cell));
    } else if (std::holds_alternative<double>(cell)) {
      reflection->SetDouble(key_msg.get(), field, std::get<double>(cell));
    } else {
      return absl::InternalError("unsupported key cell type for proto serialization");
    }
  }
  return key_msg->SerializeAsString();
}

absl::Status AddIndexRow(StorageInterface* storage, const std::string& branch, const DerivedIndexEntry& entry, uint64_t artifact_id,
                         const IndexDefinition& index_def, const google::protobuf::Descriptor& descriptor) {
  const std::string index_path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

  auto schema_or = GenerateIndexSchema(index_def, descriptor);
  if (!schema_or.ok())
    return schema_or.status();

  auto proto_key_or = BuildProtoSerializedKey(*schema_or, entry.key_values);
  if (!proto_key_or.ok())
    return proto_key_or.status();

  IndexObject index_obj;
  index_obj.serialized_key = *proto_key_or;

  auto existing_or = storage->GetObject(branch, index_path);
  if (existing_or.ok()) {
    auto deser_or = DeserializeIndexObject(*schema_or, index_def, *existing_or);
    if (deser_or.ok()) {
      index_obj = std::move(*deser_or);
    }
  }

  if (index_def.unique() && !index_obj.rows.empty()) {
    return absl::AbortedError("unique index conflict");
  }

  IndexRow row;
  row.artifact_id = artifact_id;
  row.order_values = entry.order_values;
  index_obj.rows.push_back(std::move(row));

  auto ser_or = SerializeIndexObject(*schema_or, index_def, index_obj);
  if (!ser_or.ok())
    return ser_or.status();
  return storage->PutObject(branch, index_path, *ser_or);
}

absl::Status RemoveIndexRow(StorageInterface* storage, const std::string& branch, const DerivedIndexEntry& entry, uint64_t artifact_id,
                            const IndexDefinition& index_def, const google::protobuf::Descriptor& descriptor) {
  const std::string index_path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

  auto schema_or = GenerateIndexSchema(index_def, descriptor);
  if (!schema_or.ok())
    return absl::OkStatus();

  auto existing_or = storage->GetObject(branch, index_path);
  if (!existing_or.ok())
    return absl::OkStatus();

  auto deser_or = DeserializeIndexObject(*schema_or, index_def, *existing_or);
  if (!deser_or.ok())
    return absl::OkStatus();

  auto& idx_obj = *deser_or;
  std::erase_if(idx_obj.rows, [artifact_id](const IndexRow& row) { return row.artifact_id == artifact_id; });

  auto ser_or = SerializeIndexObject(*schema_or, index_def, idx_obj);
  if (ser_or.ok()) {
    (void)storage->PutObject(branch, index_path, *ser_or);
  }
  return absl::OkStatus();
}

absl::Status DeriveAndWriteIndexEntries(StorageInterface* storage, const std::string& branch, const google::protobuf::Descriptor& descriptor,
                                        const google::protobuf::Message& message, uint64_t artifact_id,
                                        const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  auto entries_or = DeriveIndexEntries(descriptor, message, artifact_id, index_def_ids_by_key_type);
  if (!entries_or.ok())
    return entries_or.status();

  for (const auto& entry : *entries_or) {
    auto index_def = FindIndexDefinition(descriptor, entry.key_type);
    if (!index_def.has_value()) {
      return absl::InternalError(absl::StrCat("no IndexDefinition for key_type: ", entry.key_type));
    }
    auto status = AddIndexRow(storage, branch, entry, artifact_id, *index_def, descriptor);
    if (!status.ok())
      return status;
  }
  return absl::OkStatus();
}

} // namespace artifact_system::index

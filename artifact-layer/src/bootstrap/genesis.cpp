#include "bootstrap/genesis.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"

namespace artifact_system::bootstrap {
namespace {

google::protobuf::FileDescriptorSet BuildDescriptorSet(const google::protobuf::Descriptor* desc) {
  google::protobuf::FileDescriptorSet fds;
  std::set<const google::protobuf::FileDescriptor*> seen;
  std::function<void(const google::protobuf::FileDescriptor*)> add_file;
  add_file = [&](const google::protobuf::FileDescriptor* fd) {
    if (!seen.insert(fd).second)
      return;
    for (int i = 0; i < fd->dependency_count(); ++i) {
      add_file(fd->dependency(i));
    }
    fd->CopyTo(fds.add_file());
  };
  add_file(desc->file());
  return fds;
}

absl::Status WriteStoredArtifact(StorageInterface* storage, const std::string& branch, uint64_t artifact_id, uint64_t version_id, std::string_view type_name,
                                 const std::string& payload) {
  StoredArtifact envelope;
  envelope.set_envelope_version(1);
  envelope.set_version_id(version_id);
  envelope.set_type_name(type_name);
  envelope.set_payload(payload);
  return storage->PutObject(branch, encoding::ArtifactPath(artifact_id), envelope.SerializeAsString());
}

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

absl::StatusOr<std::string> BuildProtoSerializedKey(const index::GeneratedIndexSchema& schema, const std::vector<index::IndexCell>& key_values) {
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

absl::Status AddIndexRow(StorageInterface* storage, const std::string& branch, const index::DerivedIndexEntry& entry, uint64_t artifact_id,
                         const IndexDefinition& index_def, const google::protobuf::Descriptor& descriptor) {
  const std::string index_path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

  auto schema_or = index::GenerateIndexSchema(index_def, descriptor);
  if (!schema_or.ok())
    return schema_or.status();

  auto proto_key_or = BuildProtoSerializedKey(*schema_or, entry.key_values);
  if (!proto_key_or.ok())
    return proto_key_or.status();

  index::IndexObject index_obj;
  index_obj.serialized_key = *proto_key_or;

  auto existing_or = storage->GetObject(branch, index_path);
  if (existing_or.ok()) {
    auto deser_or = index::DeserializeIndexObject(*schema_or, index_def, *existing_or);
    if (deser_or.ok()) {
      index_obj = std::move(*deser_or);
    }
  }

  index::IndexRow row;
  row.artifact_id = artifact_id;
  row.order_values = entry.order_values;
  index_obj.rows.push_back(std::move(row));

  auto ser_or = index::SerializeIndexObject(*schema_or, index_def, index_obj);
  if (!ser_or.ok())
    return ser_or.status();
  return storage->PutObject(branch, index_path, *ser_or);
}

absl::Status DeriveAndWriteIndexEntries(StorageInterface* storage, const std::string& branch, const google::protobuf::Descriptor& descriptor,
                                        const google::protobuf::Message& message, uint64_t artifact_id,
                                        const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  auto entries_or = index::DeriveIndexEntries(descriptor, message, artifact_id, index_def_ids_by_key_type);
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

std::unordered_map<std::string, uint64_t> BuildIndexDefIdsMap() {
  return {
      {"index_key_type_unique", GenesisIds::kIndexKeyTypeUnique},
      {"all_index_definitions", GenesisIds::kAllIndexDefinitions},
      {"type_name_unique", GenesisIds::kTypeNameUnique},
      {"all_types", GenesisIds::kAllTypes},
      {"type_versions_by_type", GenesisIds::kTypeVersionsByType},
      {"reference_key_type_unique", GenesisIds::kReferenceKeyTypeUnique},
      {"references_by_target_type", GenesisIds::kReferencesByTargetType},
      {"all_reference_definitions", GenesisIds::kAllReferenceDefinitions},
  };
}

struct StagedArtifact {
  uint64_t artifact_id;
  const google::protobuf::Descriptor* descriptor;
  std::unique_ptr<google::protobuf::Message> message;
};

} // namespace

absl::StatusOr<GenesisResult> RunGenesis(StorageInterface* storage) {
  const std::string branch = storage->GetCanonicalBranch();
  const auto index_def_ids = BuildIndexDefIdsMap();

  // Idempotency: check committed state (not staged) by reading from the
  // branch head commit. This avoids false positives from a partial genesis
  // that staged objects but crashed before committing.
  auto head_or = storage->GetBranchHead(branch);
  if (!head_or.ok())
    return head_or.status();
  auto exists_or = storage->ObjectExists(*head_or, encoding::ArtifactPath(GenesisIds::kIndexDefinitionTypeDef));
  if (!exists_or.ok())
    return exists_or.status();
  if (*exists_or) {
    return GenesisResult{.index_def_ids_by_key_type = index_def_ids};
  }

  const auto* idx_desc = IndexDefinition::descriptor();
  const auto* td_desc = TypeDefinition::descriptor();
  const auto* tvd_desc = TypeVersionDefinition::descriptor();
  const auto* rd_desc = ReferenceDefinition::descriptor();

  std::vector<StagedArtifact> staged;

  // Helper to stage a TypeDefinition artifact.
  // All built-in types deny direct CRUD — they are managed exclusively by the registry.
  auto stage_type_def = [&](uint64_t artifact_id, uint64_t version_id, const google::protobuf::Descriptor* type_desc) -> absl::Status {
    TypeDefinition td;
    td.set_type_name(type_desc->full_name());
    td.set_current_version_id(version_id);
    td.set_deny_create(true);
    td.set_deny_update(true);
    td.set_deny_delete(true);

    std::string payload = td.SerializeAsString();
    auto status = WriteStoredArtifact(storage, branch, artifact_id, GenesisIds::kTypeDefinitionTypeVersionDef, td_desc->full_name(), payload);
    if (!status.ok())
      return status;

    auto msg = std::make_unique<TypeDefinition>(std::move(td));
    staged.push_back({artifact_id, td_desc, std::move(msg)});
    return absl::OkStatus();
  };

  // Helper to stage a TypeVersionDefinition artifact.
  auto stage_type_version_def = [&](uint64_t artifact_id, uint64_t type_id, const google::protobuf::Descriptor* type_desc) -> absl::Status {
    TypeVersionDefinition tvd;
    tvd.set_type_id(type_id);
    *tvd.mutable_descriptor_set() = BuildDescriptorSet(type_desc);
    tvd.set_proto_source("");

    std::string payload = tvd.SerializeAsString();
    auto status = WriteStoredArtifact(storage, branch, artifact_id, GenesisIds::kTypeVersionDefinitionTypeVersionDef, tvd_desc->full_name(), payload);
    if (!status.ok())
      return status;

    auto msg = std::make_unique<TypeVersionDefinition>(std::move(tvd));
    staged.push_back({artifact_id, tvd_desc, std::move(msg)});
    return absl::OkStatus();
  };

  // Helper to stage IndexDefinition artifacts from descriptor options.
  // `ids` maps key_type -> pre-allocated artifact_id for each index on this type.
  auto stage_index_defs = [&](const google::protobuf::Descriptor* type_desc, const std::unordered_map<std::string, uint64_t>& ids) -> absl::Status {
    const auto& options = type_desc->options();
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& def = options.GetExtension(artifact_system::indexes, i);
      auto it = ids.find(def.key_type());
      if (it == ids.end()) {
        return absl::InternalError(absl::StrCat("no pre-allocated ID for index key_type: ", def.key_type()));
      }
      uint64_t artifact_id = it->second;
      std::string payload = def.SerializeAsString();
      auto status = WriteStoredArtifact(storage, branch, artifact_id, GenesisIds::kIndexDefinitionTypeVersionDef, idx_desc->full_name(), payload);
      if (!status.ok())
        return status;

      auto msg = std::make_unique<IndexDefinition>(def);
      staged.push_back({artifact_id, idx_desc, std::move(msg)});
    }
    return absl::OkStatus();
  };

  // ── 1-9. Stage all four built-in types in dependency order ──
  struct BuiltInType {
    uint64_t type_def_id;
    uint64_t type_version_def_id;
    const google::protobuf::Descriptor* descriptor;
    std::unordered_map<std::string, uint64_t> index_ids;
  };

  const BuiltInType built_in_types[] = {
      {GenesisIds::kIndexDefinitionTypeDef,
       GenesisIds::kIndexDefinitionTypeVersionDef,
       idx_desc,
       {{"index_key_type_unique", GenesisIds::kIndexKeyTypeUnique}, {"all_index_definitions", GenesisIds::kAllIndexDefinitions}}},
      {GenesisIds::kTypeDefinitionTypeDef,
       GenesisIds::kTypeDefinitionTypeVersionDef,
       td_desc,
       {{"type_name_unique", GenesisIds::kTypeNameUnique}, {"all_types", GenesisIds::kAllTypes}}},
      {GenesisIds::kTypeVersionDefinitionTypeDef,
       GenesisIds::kTypeVersionDefinitionTypeVersionDef,
       tvd_desc,
       {{"type_versions_by_type", GenesisIds::kTypeVersionsByType}}},
      {GenesisIds::kReferenceDefinitionTypeDef,
       GenesisIds::kReferenceDefinitionTypeVersionDef,
       rd_desc,
       {{"reference_key_type_unique", GenesisIds::kReferenceKeyTypeUnique},
        {"references_by_target_type", GenesisIds::kReferencesByTargetType},
        {"all_reference_definitions", GenesisIds::kAllReferenceDefinitions}}},
  };

  absl::Status status;
  for (const auto& bt : built_in_types) {
    status = stage_type_def(bt.type_def_id, bt.type_version_def_id, bt.descriptor);
    if (!status.ok())
      return status;
    status = stage_type_version_def(bt.type_version_def_id, bt.type_def_id, bt.descriptor);
    if (!status.ok())
      return status;
    status = stage_index_defs(bt.descriptor, bt.index_ids);
    if (!status.ok())
      return status;
  }

  // ── 10. Built-in ReferenceDefinition: TypeVersionDefinition.type_id -> TypeDefinition ──
  {
    ReferenceDefinition ref;
    ref.set_key_type("artifact_system.TypeVersionDefinition.type_id");
    ref.set_target_type_name("artifact_system.TypeDefinition");
    ref.set_referencing_type_name("artifact_system.TypeVersionDefinition");
    ref.set_field_name("type_id");
    ref.set_covering_index_key_type("type_versions_by_type");
    ref.set_on_delete(ReferenceOption::RESTRICT);

    std::string payload = ref.SerializeAsString();
    status = WriteStoredArtifact(storage, branch, GenesisIds::kRefTypeVersionDefTypeId, GenesisIds::kReferenceDefinitionTypeVersionDef, rd_desc->full_name(),
                                 payload);
    if (!status.ok())
      return status;

    auto msg = std::make_unique<ReferenceDefinition>(std::move(ref));
    staged.push_back({GenesisIds::kRefTypeVersionDefTypeId, rd_desc, std::move(msg)});
  }

  // ── 11. Derive and write all index entries ──
  for (const auto& artifact : staged) {
    status = DeriveAndWriteIndexEntries(storage, branch, *artifact.descriptor, *artifact.message, artifact.artifact_id, index_def_ids);
    if (!status.ok())
      return status;
  }

  // ── 12. Atomic commit ──
  auto commit_or = storage->Commit(branch, "genesis: bootstrap built-in artifacts");
  if (!commit_or.ok())
    return commit_or.status();

  return GenesisResult{.index_def_ids_by_key_type = index_def_ids};
}

} // namespace artifact_system::bootstrap

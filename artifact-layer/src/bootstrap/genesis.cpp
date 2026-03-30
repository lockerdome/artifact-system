#include "bootstrap/genesis.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "index/index_utils.h"

namespace artifact_system::bootstrap {
namespace {

absl::Status WriteStoredArtifact(StorageInterface* storage, const std::string& branch, uint64_t artifact_id, uint64_t version_id, std::string_view type_name,
                                 const std::string& payload) {
  return storage->PutObject(branch, encoding::ArtifactPath(artifact_id), artifact::SerializeStoredArtifact(version_id, type_name, payload));
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
    *tvd.mutable_descriptor_set() = artifact::BuildDescriptorSet(type_desc);
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
  auto index_ids_for = [&](const google::protobuf::Descriptor* desc) {
    std::unordered_map<std::string, uint64_t> result;
    const auto& options = desc->options();
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& key = options.GetExtension(artifact_system::indexes, i).key_type();
      result[key] = index_def_ids.at(key);
    }
    return result;
  };

  struct BuiltInType {
    uint64_t type_def_id;
    uint64_t type_version_def_id;
    const google::protobuf::Descriptor* descriptor;
  };

  const BuiltInType built_in_types[] = {
      {GenesisIds::kIndexDefinitionTypeDef, GenesisIds::kIndexDefinitionTypeVersionDef, idx_desc},
      {GenesisIds::kTypeDefinitionTypeDef, GenesisIds::kTypeDefinitionTypeVersionDef, td_desc},
      {GenesisIds::kTypeVersionDefinitionTypeDef, GenesisIds::kTypeVersionDefinitionTypeVersionDef, tvd_desc},
      {GenesisIds::kReferenceDefinitionTypeDef, GenesisIds::kReferenceDefinitionTypeVersionDef, rd_desc},
  };

  absl::Status status;
  for (const auto& bt : built_in_types) {
    status = stage_type_def(bt.type_def_id, bt.type_version_def_id, bt.descriptor);
    if (!status.ok())
      return status;
    status = stage_type_version_def(bt.type_version_def_id, bt.type_def_id, bt.descriptor);
    if (!status.ok())
      return status;
    status = stage_index_defs(bt.descriptor, index_ids_for(bt.descriptor));
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
    status = index::DeriveAndWriteIndexEntries(storage, branch, *artifact.descriptor, *artifact.message, artifact.artifact_id, index_def_ids);
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

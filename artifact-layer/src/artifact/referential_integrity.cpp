#include "artifact/referential_integrity.h"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"

namespace artifact_system::artifact {
namespace {

// Validate a single reference value (a uint64 artifact_id) against storage.
absl::StatusOr<std::optional<ArtifactWriteViolation>> ValidateSingleReference(uint64_t ref_id, const std::string& field_name,
                                                                              const std::string& target_type_name, const RefIntegrityContext& ctx) {
  auto stored_or = ReadStoredArtifactIfExists(ctx.storage, ctx.ref, ref_id);
  if (!stored_or.ok()) {
    return stored_or.status();
  }

  if (!stored_or->has_value()) {
    return MakeViolation(
        ArtifactWriteViolation::REFERENCE_TARGET_NOT_FOUND, absl::StrCat("field: ", field_name),
        absl::StrCat("reference field '", field_name, "' targets artifact_id ", ref_id, " which does not exist (expected type '", target_type_name, "')"));
  }

  const StoredArtifact& stored = stored_or->value();

  // Empty payload means tombstone.
  if (stored.payload().empty()) {
    return MakeViolation(
        ArtifactWriteViolation::REFERENCE_TARGET_TOMBSTONED, absl::StrCat("field: ", field_name),
        absl::StrCat("reference field '", field_name, "' targets artifact_id ", ref_id, " which is tombstoned (expected type '", target_type_name, "')"));
  }

  // Type check.
  if (stored.type_name() != target_type_name) {
    return MakeViolation(ArtifactWriteViolation::REFERENCE_TARGET_WRONG_TYPE, absl::StrCat("field: ", field_name),
                         absl::StrCat("reference field '", field_name, "' targets artifact_id ", ref_id, " with type '", stored.type_name(), "', expected '",
                                      target_type_name, "'"));
  }

  return std::nullopt; // valid
}

// Read an index object and extract the artifact_ids from its rows.
// Returns an empty vector if the index object does not exist.
absl::StatusOr<std::vector<uint64_t>> ReadIndexArtifactIds(uint64_t index_def_id, const std::vector<uint8_t>& encoded_key,
                                                           const IndexDefinition& index_definition, const google::protobuf::Descriptor& parent_descriptor,
                                                           const RefIntegrityContext& ctx) {
  const std::string path = encoding::IndexPath(index_def_id, encoded_key);
  auto data_or = ctx.storage->GetObject(ctx.ref, path);
  if (!data_or.ok()) {
    if (absl::IsNotFound(data_or.status())) {
      return std::vector<uint64_t>{};
    }
    return data_or.status();
  }

  auto schema_or = index::GenerateIndexSchema(index_definition, parent_descriptor);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  auto object_or = index::DeserializeIndexObject(*schema_or, index_definition, *data_or);
  if (!object_or.ok()) {
    return object_or.status();
  }

  std::vector<uint64_t> ids;
  ids.reserve(object_or->rows.size());
  for (const auto& row : object_or->rows) {
    ids.push_back(row.artifact_id);
  }
  return ids;
}

// Build an IndexDefinition proto from its fields (for index lookup).
IndexDefinition MakeIndexDefinition(const std::string& key_type, const std::vector<std::string>& key_fields, bool unique) {
  IndexDefinition def;
  def.set_key_type(key_type);
  for (const auto& k : key_fields) {
    def.add_key(k);
  }
  auto* order = def.add_order();
  order->set_field("artifact_id");
  order->set_direction(OrderDefinition::ASCENDING);
  def.set_unique(unique);
  return def;
}

// Encode a single uint64 value as an index key (for covering index lookups).
std::vector<uint8_t> EncodeUint64Key(uint64_t value) {
  // Encode as a uint64 field: 8 bytes little-endian (matching EncodeIndexCell).
  std::vector<uint8_t> out;
  out.reserve(8);
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFU));
  }
  return out;
}

// Encode a string value as an index key (varint length + bytes).
std::vector<uint8_t> EncodeStringKey(const std::string& value) {
  auto length_bytes = encoding::EncodeVarint(value.size());
  std::vector<uint8_t> out;
  out.reserve(length_bytes.size() + value.size());
  out.insert(out.end(), length_bytes.begin(), length_bytes.end());
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

// Resolve the referencing type's descriptor for a ReferenceDefinition.
// Path: referencing_type_name → type_name_unique index → TypeDefinition
//       → current_version_id → TypeVersionDefinition → descriptor_set.
// Returns nullptr if any step fails (best-effort).
struct ResolvedReferencingType {
  google::protobuf::FileDescriptorSet descriptor_set;
  std::unique_ptr<google::protobuf::DescriptorPool> pool;
  const google::protobuf::Descriptor* descriptor = nullptr;
};

std::optional<ResolvedReferencingType> ResolveReferencingType(const std::string& referencing_type_name, const RefIntegrityContext& ctx,
                                                              const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  // Step 1: Find the type_name_unique index to look up the TypeDefinition.
  auto tnu_it = index_def_ids_by_key_type.find("type_name_unique");
  if (tnu_it == index_def_ids_by_key_type.end()) {
    return std::nullopt;
  }

  const auto* td_descriptor = TypeDefinition::descriptor();
  const IndexDefinition tnu_def = MakeIndexDefinition("type_name_unique", {"type_name"}, true);
  const std::vector<uint8_t> tnu_key = EncodeStringKey(referencing_type_name);

  auto td_ids_or = ReadIndexArtifactIds(tnu_it->second, tnu_key, tnu_def, *td_descriptor, ctx);
  if (!td_ids_or.ok() || td_ids_or->empty()) {
    return std::nullopt;
  }
  const uint64_t type_def_id = (*td_ids_or)[0];

  // Step 2: Read the TypeDefinition to get current_version_id.
  auto td_stored_or = ReadStoredArtifactIfExists(ctx.storage, ctx.ref, type_def_id);
  if (!td_stored_or.ok() || !td_stored_or->has_value() || td_stored_or->value().payload().empty()) {
    return std::nullopt;
  }
  TypeDefinition td;
  if (!td.ParseFromString(td_stored_or->value().payload())) {
    return std::nullopt;
  }

  // Step 3: Read the TypeVersionDefinition to get the descriptor_set.
  auto tvd_stored_or = ReadStoredArtifactIfExists(ctx.storage, ctx.ref, td.current_version_id());
  if (!tvd_stored_or.ok() || !tvd_stored_or->has_value() || tvd_stored_or->value().payload().empty()) {
    return std::nullopt;
  }
  TypeVersionDefinition tvd;
  if (!tvd.ParseFromString(tvd_stored_or->value().payload())) {
    return std::nullopt;
  }

  // Step 4: Build pool and find the message descriptor.
  ResolvedReferencingType result;
  result.descriptor_set = tvd.descriptor_set();
  result.pool = std::make_unique<google::protobuf::DescriptorPool>(google::protobuf::DescriptorPool::generated_pool());
  result.descriptor = BuildPoolAndFindMessage(result.descriptor_set, referencing_type_name, result.pool.get());
  if (result.descriptor == nullptr) {
    return std::nullopt;
  }
  return result;
}

// Find artifact_ids that actively reference the given artifact_id through a
// specific ReferenceDefinition's covering index. Returns only IDs not already
// in scheduled_deletes and not the artifact itself.
absl::StatusOr<std::vector<uint64_t>> FindActiveReferencingArtifacts(uint64_t artifact_id, const ReferenceDefinition& ref_def, const RefIntegrityContext& ctx,
                                                                     const std::set<uint64_t>& scheduled_deletes,
                                                                     const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  const std::string& covering_key_type = ref_def.covering_index_key_type();
  const auto covering_idx_it = index_def_ids_by_key_type.find(covering_key_type);
  if (covering_idx_it == index_def_ids_by_key_type.end()) {
    return std::vector<uint64_t>{};
  }

  const uint64_t covering_idx_def_id = covering_idx_it->second;
  const std::vector<uint8_t> covering_encoded_key = EncodeUint64Key(artifact_id);
  const IndexDefinition covering_index_def = MakeIndexDefinition(covering_key_type, {ref_def.field_name()}, false);

  auto resolved = ResolveReferencingType(ref_def.referencing_type_name(), ctx, index_def_ids_by_key_type);
  if (!resolved.has_value()) {
    return std::vector<uint64_t>{};
  }

  auto referencing_ids_or = ReadIndexArtifactIds(covering_idx_def_id, covering_encoded_key, covering_index_def, *resolved->descriptor, ctx);
  if (!referencing_ids_or.ok()) {
    if (absl::IsNotFound(referencing_ids_or.status()) || absl::IsInvalidArgument(referencing_ids_or.status())) {
      return std::vector<uint64_t>{};
    }
    return referencing_ids_or.status();
  }

  std::vector<uint64_t> active;
  for (const uint64_t ref_artifact_id : *referencing_ids_or) {
    if (scheduled_deletes.count(ref_artifact_id) == 0 && ref_artifact_id != artifact_id) {
      active.push_back(ref_artifact_id);
    }
  }
  return active;
}

} // namespace

// Validate reference fields on a single message level (non-recursive).
// Appends violations to `violations`. `field_path_prefix` is the dotted path
// from the root message to the current message (empty for the root itself).
void ValidateReferencesOnFields(const google::protobuf::Message& message, const google::protobuf::Descriptor& descriptor, const RefIntegrityContext& ctx,
                                const std::string& field_path_prefix, std::vector<ArtifactWriteViolation>& violations, absl::Status& out_status) {
  const auto* reflection = message.GetReflection();

  for (int i = 0; i < descriptor.field_count(); ++i) {
    const auto* field = descriptor.field(i);
    const std::string field_path = field_path_prefix.empty() ? std::string(field->name()) : absl::StrCat(field_path_prefix, ".", field->name());

    if (field->options().HasExtension(artifact_system::references)) {
      const ReferenceOption& ref_opt = field->options().GetExtension(artifact_system::references);
      const std::string& target_type_name = ref_opt.target_type_name();

      // Reference fields must be uint64.
      if (field->type() != google::protobuf::FieldDescriptor::TYPE_UINT64) {
        continue;
      }

      if (field->is_repeated()) {
        // Repeated uint64 reference field.
        const int count = reflection->FieldSize(message, field);

        // Check for duplicates first.
        std::unordered_set<uint64_t> seen;
        seen.reserve(static_cast<size_t>(count));
        for (int j = 0; j < count; ++j) {
          const uint64_t ref_id = reflection->GetRepeatedUInt64(message, field, j);
          if (!seen.insert(ref_id).second) {
            violations.push_back(MakeViolation(ArtifactWriteViolation::REFERENCE_DUPLICATE_VALUE, absl::StrCat("field: ", field_path),
                                               absl::StrCat("repeated reference field '", field_path, "' contains duplicate artifact_id ", ref_id)));
          }
        }

        // Validate each value.
        std::unordered_set<uint64_t> validated;
        validated.reserve(static_cast<size_t>(count));
        for (int j = 0; j < count; ++j) {
          const uint64_t ref_id = reflection->GetRepeatedUInt64(message, field, j);
          if (!validated.insert(ref_id).second) {
            continue;
          }
          auto result_or = ValidateSingleReference(ref_id, field_path, target_type_name, ctx);
          if (!result_or.ok()) {
            out_status = result_or.status();
            return;
          }
          if (result_or->has_value()) {
            violations.push_back(result_or->value());
          }
        }
      } else {
        // Scalar uint64 reference field.
        if (field->has_presence() && !reflection->HasField(message, field)) {
          continue;
        }
        const uint64_t ref_id = reflection->GetUInt64(message, field);
        auto result_or = ValidateSingleReference(ref_id, field_path, target_type_name, ctx);
        if (!result_or.ok()) {
          out_status = result_or.status();
          return;
        }
        if (result_or->has_value()) {
          violations.push_back(result_or->value());
        }
      }
    } else if (field->message_type() != nullptr && field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
      // Recurse into nested message fields to find nested reference annotations.
      if (field->is_repeated()) {
        const int count = reflection->FieldSize(message, field);
        for (int j = 0; j < count; ++j) {
          const auto& sub_msg = reflection->GetRepeatedMessage(message, field, j);
          ValidateReferencesOnFields(sub_msg, *field->message_type(), ctx, field_path, violations, out_status);
          if (!out_status.ok())
            return;
        }
      } else {
        if (field->has_presence() && !reflection->HasField(message, field)) {
          continue;
        }
        const auto& sub_msg = reflection->GetMessage(message, field);
        ValidateReferencesOnFields(sub_msg, *field->message_type(), ctx, field_path, violations, out_status);
        if (!out_status.ok())
          return;
      }
    }
  }
}

absl::StatusOr<std::vector<ArtifactWriteViolation>> ValidateReferences(const google::protobuf::Message& message, const google::protobuf::Descriptor& descriptor,
                                                                       const RefIntegrityContext& ctx) {
  if (message.GetDescriptor() != &descriptor) {
    return absl::InvalidArgumentError("message descriptor mismatch");
  }

  std::vector<ArtifactWriteViolation> violations;
  absl::Status status = absl::OkStatus();
  ValidateReferencesOnFields(message, descriptor, ctx, "", violations, status);
  if (!status.ok())
    return status;
  return violations;
}

absl::StatusOr<DeleteEnforcementResult> EnforceDeleteIntegrity(uint64_t artifact_id, const std::string& type_name, const RefIntegrityContext& ctx,
                                                               const std::set<uint64_t>& scheduled_deletes,
                                                               const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  DeleteEnforcementResult result;

  // Step 1: Find all ReferenceDefinitions where target_type_name == type_name.
  // Use the "references_by_target_type" index.
  const auto refs_idx_it = index_def_ids_by_key_type.find("references_by_target_type");
  if (refs_idx_it == index_def_ids_by_key_type.end()) {
    return result;
  }

  const uint64_t refs_index_def_id = refs_idx_it->second;
  const std::vector<uint8_t> refs_encoded_key = EncodeStringKey(type_name);
  const IndexDefinition refs_index_def = MakeIndexDefinition("references_by_target_type", {"target_type_name"}, false);
  const auto* ref_def_descriptor = ReferenceDefinition::descriptor();

  auto ref_def_ids_or = ReadIndexArtifactIds(refs_index_def_id, refs_encoded_key, refs_index_def, *ref_def_descriptor, ctx);
  if (!ref_def_ids_or.ok()) {
    return ref_def_ids_or.status();
  }

  // Step 2: For each ReferenceDefinition, read it and enforce the policy.
  for (const uint64_t ref_def_id : *ref_def_ids_or) {
    auto stored_or = ReadStoredArtifactIfExists(ctx.storage, ctx.ref, ref_def_id);
    if (!stored_or.ok()) {
      return stored_or.status();
    }
    if (!stored_or->has_value() || stored_or->value().payload().empty()) {
      continue;
    }

    ReferenceDefinition ref_def;
    if (!ref_def.ParseFromString(stored_or->value().payload())) {
      return absl::InternalError(absl::StrCat("failed to parse ReferenceDefinition artifact ", ref_def_id));
    }

    if (ref_def.target_type_name() != type_name) {
      continue;
    }

    // Step 3: Find artifacts that actively reference this one via the covering index.
    auto active_or = FindActiveReferencingArtifacts(artifact_id, ref_def, ctx, scheduled_deletes, index_def_ids_by_key_type);
    if (!active_or.ok()) {
      return active_or.status();
    }
    const std::vector<uint64_t>& active_referencing = *active_or;

    if (active_referencing.empty()) {
      continue;
    }

    switch (ref_def.on_delete()) {
    case ReferenceOption::RESTRICT:
      result.violations.push_back(
          MakeViolation(ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED, absl::StrCat("reference: ", ref_def.key_type()),
                        absl::StrCat("delete restricted: ", active_referencing.size(), " referencing artifact(s) exist via ", ref_def.key_type())));
      break;

    case ReferenceOption::CASCADE: {
      // Recursively enforce referential integrity on cascaded artifacts
      // (PRD: "recursively applying referential integrity").
      for (const uint64_t ref_artifact_id : active_referencing) {
        result.side_effects.push_back(CascadeDelete{ref_artifact_id});
      }
      // Recurse: each cascaded artifact may itself have references.
      std::set<uint64_t> updated_scheduled = scheduled_deletes;
      for (const uint64_t ref_artifact_id : active_referencing) {
        updated_scheduled.insert(ref_artifact_id);
      }
      for (const uint64_t ref_artifact_id : active_referencing) {
        // Read the cascaded artifact to get its type_name.
        auto cascade_stored_or = ReadStoredArtifactIfExists(ctx.storage, ctx.ref, ref_artifact_id);
        if (!cascade_stored_or.ok() || !cascade_stored_or->has_value() || cascade_stored_or->value().payload().empty()) {
          continue;
        }
        auto cascade_result_or =
            EnforceDeleteIntegrity(ref_artifact_id, cascade_stored_or->value().type_name(), ctx, updated_scheduled, index_def_ids_by_key_type);
        if (!cascade_result_or.ok()) {
          return cascade_result_or.status();
        }
        // Propagate violations and side effects from the recursive call.
        result.violations.insert(result.violations.end(), cascade_result_or->violations.begin(), cascade_result_or->violations.end());
        result.side_effects.insert(result.side_effects.end(), cascade_result_or->side_effects.begin(), cascade_result_or->side_effects.end());
        // Track all newly scheduled deletes from the recursive call.
        for (const auto& effect : cascade_result_or->side_effects) {
          if (std::holds_alternative<CascadeDelete>(effect)) {
            updated_scheduled.insert(std::get<CascadeDelete>(effect).artifact_id);
          }
        }
      }
      break;
    }

    case ReferenceOption::SET_NULL:
      for (const uint64_t ref_artifact_id : active_referencing) {
        result.side_effects.push_back(SetNullUpdate{ref_artifact_id, ref_def.field_name(), artifact_id});
      }
      break;

    default:
      result.violations.push_back(
          MakeViolation(ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED, absl::StrCat("reference: ", ref_def.key_type()),
                        absl::StrCat("delete restricted: ", active_referencing.size(), " referencing artifact(s) exist via ", ref_def.key_type())));
      break;
    }
  }

  return result;
}

} // namespace artifact_system::artifact

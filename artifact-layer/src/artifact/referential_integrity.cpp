#include "artifact/referential_integrity.h"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"

namespace artifact_system::artifact {
namespace {

// Helper: read a StoredArtifact from storage by artifact_id.
// Returns nullopt if the object does not exist (NOT_FOUND).
absl::StatusOr<std::optional<StoredArtifact>> ReadStoredArtifact(uint64_t artifact_id, const RefIntegrityContext& ctx) {
  const std::string path = encoding::ArtifactPath(artifact_id);
  auto data_or = ctx.storage->GetObject(ctx.ref, path);
  if (!data_or.ok()) {
    if (absl::IsNotFound(data_or.status())) {
      return std::nullopt;
    }
    return data_or.status();
  }
  StoredArtifact stored;
  if (!stored.ParseFromString(*data_or)) {
    return absl::InternalError(absl::StrCat("failed to parse StoredArtifact at ", path));
  }
  return stored;
}

// Helper: create a violation.
ArtifactWriteViolation MakeViolation(ArtifactWriteViolation::Category category, const std::string& subject, const std::string& description) {
  ArtifactWriteViolation v;
  v.set_category(category);
  v.set_subject(subject);
  v.set_description(description);
  return v;
}

// Validate a single reference value (a uint64 artifact_id) against storage.
absl::StatusOr<std::optional<ArtifactWriteViolation>> ValidateSingleReference(uint64_t ref_id, const std::string& field_name,
                                                                              const std::string& target_type_name, const RefIntegrityContext& ctx) {
  auto stored_or = ReadStoredArtifact(ref_id, ctx);
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

} // namespace

absl::StatusOr<std::vector<ArtifactWriteViolation>> ValidateReferences(const google::protobuf::Message& message, const google::protobuf::Descriptor& descriptor,
                                                                       const RefIntegrityContext& ctx) {
  if (message.GetDescriptor() != &descriptor) {
    return absl::InvalidArgumentError("message descriptor mismatch");
  }

  std::vector<ArtifactWriteViolation> violations;

  for (int i = 0; i < descriptor.field_count(); ++i) {
    const auto* field = descriptor.field(i);

    // Check for the references extension option.
    if (!field->options().HasExtension(artifact_system::references)) {
      continue;
    }
    const ReferenceOption& ref_opt = field->options().GetExtension(artifact_system::references);
    const std::string& target_type_name = ref_opt.target_type_name();

    // Reference fields must be uint64.
    if (field->type() != google::protobuf::FieldDescriptor::TYPE_UINT64) {
      continue;
    }

    const auto* reflection = message.GetReflection();

    if (field->is_repeated()) {
      // Repeated uint64 reference field.
      const int count = reflection->FieldSize(message, field);

      // Check for duplicates first.
      std::unordered_set<uint64_t> seen;
      seen.reserve(static_cast<size_t>(count));
      for (int j = 0; j < count; ++j) {
        const uint64_t ref_id = reflection->GetRepeatedUInt64(message, field, j);
        if (!seen.insert(ref_id).second) {
          violations.push_back(MakeViolation(ArtifactWriteViolation::REFERENCE_DUPLICATE_VALUE, absl::StrCat("field: ", field->name()),
                                             absl::StrCat("repeated reference field '", field->name(), "' contains duplicate artifact_id ", ref_id)));
        }
      }

      // Validate each value.
      std::unordered_set<uint64_t> validated;
      validated.reserve(static_cast<size_t>(count));
      for (int j = 0; j < count; ++j) {
        const uint64_t ref_id = reflection->GetRepeatedUInt64(message, field, j);
        if (!validated.insert(ref_id).second) {
          continue; // already validated this ID
        }
        auto result_or = ValidateSingleReference(ref_id, std::string(field->name()), target_type_name, ctx);
        if (!result_or.ok()) {
          return result_or.status();
        }
        if (result_or->has_value()) {
          violations.push_back(result_or->value());
        }
      }
    } else {
      // Scalar uint64 reference field.
      // For optional (has_presence), skip if not set.
      if (field->has_presence() && !reflection->HasField(message, field)) {
        continue;
      }

      // For implicit-presence scalar, always validate (including default 0).
      const uint64_t ref_id = reflection->GetUInt64(message, field);
      auto result_or = ValidateSingleReference(ref_id, std::string(field->name()), target_type_name, ctx);
      if (!result_or.ok()) {
        return result_or.status();
      }
      if (result_or->has_value()) {
        violations.push_back(result_or->value());
      }
    }
  }

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
    // No index definition ID provided for references_by_target_type.
    // This means no reference definitions are registered, so nothing to enforce.
    return result;
  }

  const uint64_t refs_index_def_id = refs_idx_it->second;

  // Encode the key for the references_by_target_type index: key is
  // [target_type_name] (a string field).
  const std::vector<uint8_t> refs_encoded_key = EncodeStringKey(type_name);

  // Build the IndexDefinition for "references_by_target_type" to use with
  // GenerateIndexSchema and DeserializeIndexObject.
  const IndexDefinition refs_index_def = MakeIndexDefinition("references_by_target_type", {"target_type_name"}, false);

  // We need the ReferenceDefinition descriptor for GenerateIndexSchema.
  const auto* ref_def_descriptor = ReferenceDefinition::descriptor();

  auto ref_def_ids_or = ReadIndexArtifactIds(refs_index_def_id, refs_encoded_key, refs_index_def, *ref_def_descriptor, ctx);
  if (!ref_def_ids_or.ok()) {
    return ref_def_ids_or.status();
  }

  // Step 2: For each ReferenceDefinition, read it and enforce the policy.
  for (const uint64_t ref_def_id : *ref_def_ids_or) {
    auto stored_or = ReadStoredArtifact(ref_def_id, ctx);
    if (!stored_or.ok()) {
      return stored_or.status();
    }
    if (!stored_or->has_value() || stored_or->value().payload().empty()) {
      continue; // missing or tombstoned reference definition, skip
    }

    ReferenceDefinition ref_def;
    if (!ref_def.ParseFromString(stored_or->value().payload())) {
      return absl::InternalError(absl::StrCat("failed to parse ReferenceDefinition artifact ", ref_def_id));
    }

    // Verify this reference definition actually targets our type.
    if (ref_def.target_type_name() != type_name) {
      continue;
    }

    // Step 3: Look up the covering index to find referencing artifacts.
    const std::string& covering_key_type = ref_def.covering_index_key_type();
    const auto covering_idx_it = index_def_ids_by_key_type.find(covering_key_type);
    if (covering_idx_it == index_def_ids_by_key_type.end()) {
      // Cannot look up the covering index; skip this reference.
      continue;
    }
    const uint64_t covering_idx_def_id = covering_idx_it->second;

    // The covering index key is the deleted artifact_id encoded as uint64.
    const std::vector<uint8_t> covering_encoded_key = EncodeUint64Key(artifact_id);

    // Build an IndexDefinition for the covering index.
    // The covering index has key = [field_name] which is a uint64 field
    // referencing the target artifact.
    const IndexDefinition covering_index_def = MakeIndexDefinition(covering_key_type, {ref_def.field_name()}, false);

    // We need the referencing type's descriptor for GenerateIndexSchema.
    // Since we don't have the dynamic descriptor for the referencing type,
    // we use the ReferenceDefinition's referencing_type_name to find it.
    // However, the covering index was registered against the referencing type.
    // For now, we read the index directly using the covering index definition
    // and the ReferenceDefinition descriptor as a proxy (the covering index
    // only has key=[uint64 field] and order=[artifact_id ASC], so any message
    // with a uint64 field at the right path works).
    //
    // A cleaner approach: the covering index is keyed by a uint64 field, so
    // we can use the ReferenceDefinition descriptor itself (which also has
    // uint64 fields) as a stand-in for schema generation when the key field
    // is a simple uint64.  But GenerateIndexSchema needs the field_name to
    // exist on the descriptor.  Instead, we just read the raw index object
    // path and deserialize it.
    //
    // Simplification: read the index object directly at the path and
    // deserialize using a synthetic schema.  The covering index key is a
    // single uint64 field, so we construct a minimal IndexDefinition and
    // use ReferenceDefinition's descriptor which has the needed uint64 field
    // at "artifact_id" position... but that won't work for arbitrary field
    // names.
    //
    // Best approach: use the index path directly and try to deserialize.
    // We construct a covering IndexDefinition with key=[field_name]. Since
    // field_name refers to a field on the *referencing type's* descriptor
    // (not ReferenceDefinition), we need that descriptor. We don't have it
    // at this layer. So we use a two-step approach:
    //   1. Read the raw bytes at the index path
    //   2. Use a generic single-uint64-key index schema for deserialization

    // For the covering index, construct an IndexDefinition that references
    // the "type_id" field from ReferenceDefinition (a uint64), since we
    // just need a descriptor with a uint64 field at position matching the
    // key field. The actual key encoding uses the raw uint64 value regardless
    // of field name, so this works for deserialization.
    const IndexDefinition covering_deser_def = MakeIndexDefinition(covering_key_type, {"type_id"}, false);

    auto referencing_ids_or = ReadIndexArtifactIds(covering_idx_def_id, covering_encoded_key, covering_deser_def, *ref_def_descriptor, ctx);
    if (!referencing_ids_or.ok()) {
      // If the index doesn't exist or can't be read, skip.
      if (absl::IsNotFound(referencing_ids_or.status()) || absl::IsInvalidArgument(referencing_ids_or.status())) {
        continue;
      }
      return referencing_ids_or.status();
    }

    // Filter out artifacts already scheduled for deletion.
    std::vector<uint64_t> active_referencing;
    for (const uint64_t ref_artifact_id : *referencing_ids_or) {
      if (scheduled_deletes.count(ref_artifact_id) == 0 && ref_artifact_id != artifact_id) {
        active_referencing.push_back(ref_artifact_id);
      }
    }

    if (active_referencing.empty()) {
      continue;
    }

    // Apply on_delete policy.
    switch (ref_def.on_delete()) {
    case ReferenceOption::RESTRICT:
      result.violations.push_back(
          MakeViolation(ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED, absl::StrCat("reference: ", ref_def.key_type()),
                        absl::StrCat("delete restricted: ", active_referencing.size(), " referencing artifact(s) exist via ", ref_def.key_type())));
      break;

    case ReferenceOption::CASCADE:
      for (const uint64_t ref_artifact_id : active_referencing) {
        result.side_effects.push_back(CascadeDelete{ref_artifact_id});
      }
      break;

    case ReferenceOption::SET_NULL:
      for (const uint64_t ref_artifact_id : active_referencing) {
        result.side_effects.push_back(SetNullUpdate{ref_artifact_id, ref_def.field_name(), artifact_id});
      }
      break;

    default:
      // ON_DELETE_UNSPECIFIED or unknown: treat as RESTRICT.
      result.violations.push_back(
          MakeViolation(ArtifactWriteViolation::REFERENCE_DELETE_RESTRICTED, absl::StrCat("reference: ", ref_def.key_type()),
                        absl::StrCat("delete restricted: ", active_referencing.size(), " referencing artifact(s) exist via ", ref_def.key_type())));
      break;
    }
  }

  return result;
}

} // namespace artifact_system::artifact

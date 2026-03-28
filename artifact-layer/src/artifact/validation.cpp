#include "artifact/validation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "index/index_derivation.h"

namespace artifact_system::artifact {
namespace {

// Helper: build a single ArtifactWriteViolation.
ArtifactWriteViolation MakeViolation(ArtifactWriteViolation::Category category, const std::string& subject, const std::string& description) {
  ArtifactWriteViolation v;
  v.set_category(category);
  v.set_subject(subject);
  v.set_description(description);
  return v;
}

// Build a DescriptorPool from a FileDescriptorSet and find a message by name.
// Returns nullptr on failure.
const google::protobuf::Descriptor* BuildPoolAndFindMessage(const google::protobuf::FileDescriptorSet& descriptor_set, const std::string& message_full_name,
                                                            google::protobuf::DescriptorPool* pool) {
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
      if (pool->FindFileByName(file.name()) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
        continue;
      }
      if (pool->BuildFile(file) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
      }
    }
  }
  return pool->FindMessageTypeByName(message_full_name);
}

// Read and parse a StoredArtifact from storage at the given artifact_id.
absl::StatusOr<StoredArtifact> ReadStoredArtifact(uint64_t artifact_id, const ValidationContext& ctx) {
  const std::string path = encoding::ArtifactPath(artifact_id);
  auto data_or = ctx.storage->GetObject(ctx.ref, path);
  if (!data_or.ok()) {
    return data_or.status();
  }
  StoredArtifact stored;
  if (!stored.ParseFromString(*data_or)) {
    return absl::InternalError(absl::StrCat("failed to parse StoredArtifact at ", path));
  }
  return stored;
}

// Validate payload against the type's descriptor set using DynamicMessage.
// Returns nullopt on success, or a violation on failure.
std::optional<ArtifactWriteViolation> ValidatePayloadParsing(const google::protobuf::FileDescriptorSet& descriptor_set, const std::string& type_name,
                                                             const std::string& payload) {
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return MakeViolation(ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE, absl::StrCat("type: ", type_name),
                         absl::StrCat("message '", type_name, "' not found in descriptor set"));
  }

  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* prototype = factory.GetPrototype(descriptor);
  if (prototype == nullptr) {
    return MakeViolation(ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE, absl::StrCat("type: ", type_name),
                         "failed to construct dynamic message prototype");
  }
  std::unique_ptr<google::protobuf::Message> message(prototype->New());
  if (!message->ParseFromString(payload)) {
    return MakeViolation(ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE, absl::StrCat("type: ", type_name),
                         "failed to parse payload bytes against type schema");
  }
  return std::nullopt;
}

// Run index derivation and translate errors into violations.
std::vector<ArtifactWriteViolation> ValidateIndexDerivation(const google::protobuf::FileDescriptorSet& descriptor_set, const std::string& type_name,
                                                            const std::string& payload,
                                                            const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  std::vector<ArtifactWriteViolation> violations;
  // Use artifact_id = 0 for validation-only derivation.
  auto result = index::DeriveIndexEntriesFromPayload(descriptor_set, type_name, payload, /*artifact_id=*/0, index_def_ids_by_key_type);
  if (result.ok()) {
    return violations;
  }

  const std::string error_msg = std::string(result.status().message());
  if (error_msg.find("NAN_IN_INDEXED_FIELD") != std::string::npos) {
    // Extract field name after "NAN_IN_INDEXED_FIELD: "
    std::string field_name = error_msg;
    const std::string prefix = "NAN_IN_INDEXED_FIELD: ";
    auto pos = field_name.find(prefix);
    if (pos != std::string::npos) {
      field_name = field_name.substr(pos + prefix.size());
    }
    violations.push_back(MakeViolation(ArtifactWriteViolation::NAN_IN_INDEXED_FIELD, absl::StrCat("field: ", field_name),
                                       absl::StrCat("NaN in indexed field '", field_name, "'")));
  } else if (error_msg.find("NON_MINIMAL_VARINT") != std::string::npos) {
    violations.push_back(MakeViolation(ArtifactWriteViolation::NON_MINIMAL_VARINT, "field: unknown", "non-minimal varint encoding in indexed field"));
  } else {
    // Unexpected index derivation error — propagate as a generic violation.
    violations.push_back(MakeViolation(ArtifactWriteViolation::PAYLOAD_VALIDATION_FAILURE, absl::StrCat("type: ", type_name),
                                       absl::StrCat("index derivation error: ", error_msg)));
  }
  return violations;
}

} // namespace

absl::StatusOr<ResolvedType> ResolveVersionId(uint64_t version_id, const ValidationContext& ctx) {
  // Step 1: Read the TypeVersionDefinition artifact.
  auto version_stored_or = ReadStoredArtifact(version_id, ctx);
  if (!version_stored_or.ok()) {
    return version_stored_or.status();
  }
  const StoredArtifact& version_stored = *version_stored_or;

  // Step 2: Verify it is a TypeVersionDefinition.
  if (version_stored.type_name() != "TypeVersionDefinition" && version_stored.type_name() != "artifact_system.TypeVersionDefinition") {
    return absl::InvalidArgumentError(absl::StrCat("version_id ", version_id, " does not resolve to a valid TypeVersionDefinition"));
  }

  // Step 3: Parse payload as TypeVersionDefinition.
  TypeVersionDefinition tvd;
  if (!tvd.ParseFromString(version_stored.payload())) {
    return absl::InvalidArgumentError(absl::StrCat("version_id ", version_id, " does not resolve to a valid TypeVersionDefinition"));
  }

  // Step 4: Read the parent TypeDefinition artifact.
  auto type_stored_or = ReadStoredArtifact(tvd.type_id(), ctx);
  if (!type_stored_or.ok()) {
    return type_stored_or.status();
  }
  const StoredArtifact& type_stored = *type_stored_or;

  // Step 5: Verify it is a TypeDefinition.
  if (type_stored.type_name() != "TypeDefinition" && type_stored.type_name() != "artifact_system.TypeDefinition") {
    return absl::InvalidArgumentError(absl::StrCat("version_id ", version_id, " does not resolve to a valid TypeVersionDefinition"));
  }

  // Step 6: Parse payload as TypeDefinition.
  TypeDefinition td;
  if (!td.ParseFromString(type_stored.payload())) {
    return absl::InvalidArgumentError(absl::StrCat("version_id ", version_id, " does not resolve to a valid TypeVersionDefinition"));
  }

  // Step 7: Build and return ResolvedType.
  ResolvedType resolved;
  resolved.type_def_id = tvd.type_id();
  resolved.type_name = td.type_name();
  resolved.deny_create = td.deny_create();
  resolved.deny_update = td.deny_update();
  resolved.deny_delete = td.deny_delete();
  resolved.version_id = version_id;
  *resolved.descriptor_set.mutable_file() = tvd.descriptor_set().file();
  return resolved;
}

absl::StatusOr<ValidationResult> ValidateCreateOrUpdate(WriteOperation op, uint64_t version_id, const std::string& payload, const ValidationContext& ctx,
                                                        std::optional<uint64_t> existing_artifact_id,
                                                        const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type) {
  ValidationResult result;

  // ── Phase 1: INVALID_VERSION_ID ───────────────────────────────────────────
  auto resolved_or = ResolveVersionId(version_id, ctx);
  if (!resolved_or.ok()) {
    // Distinguish between I/O errors (NOT_FOUND from storage is a validation
    // failure) and truly unexpected errors.
    if (absl::IsNotFound(resolved_or.status()) || absl::IsInvalidArgument(resolved_or.status())) {
      result.violations.push_back(MakeViolation(ArtifactWriteViolation::INVALID_VERSION_ID, absl::StrCat("version_id: ", version_id),
                                                absl::StrCat("version_id ", version_id, " does not resolve to a valid TypeVersionDefinition")));
      return result; // short-circuit
    }
    return resolved_or.status(); // unexpected error
  }
  const ResolvedType& resolved = *resolved_or;

  // For Update: verify type_name matches the existing artifact.
  if (op == WriteOperation::kUpdate && existing_artifact_id.has_value()) {
    auto existing_or = ReadStoredArtifact(*existing_artifact_id, ctx);
    if (!existing_or.ok()) {
      if (absl::IsNotFound(existing_or.status())) {
        result.violations.push_back(MakeViolation(ArtifactWriteViolation::INVALID_VERSION_ID, absl::StrCat("version_id: ", version_id),
                                                  absl::StrCat("type_name mismatch on update: existing artifact ", *existing_artifact_id, " not found")));
        return result; // short-circuit
      }
      return existing_or.status();
    }
    if (existing_or->type_name() != resolved.type_name) {
      result.violations.push_back(
          MakeViolation(ArtifactWriteViolation::INVALID_VERSION_ID, absl::StrCat("version_id: ", version_id),
                        absl::StrCat("type_name mismatch on update: expected '", existing_or->type_name(), "', got '", resolved.type_name, "'")));
      return result; // short-circuit
    }
  }

  // ── Phase 2: MUTATION_DENIED ──────────────────────────────────────────────
  if (!ctx.bypass_mutation_check) {
    if (op == WriteOperation::kCreate && resolved.deny_create) {
      result.violations.push_back(MakeViolation(ArtifactWriteViolation::MUTATION_DENIED, absl::StrCat("type: ", resolved.type_name),
                                                absl::StrCat("CreateArtifact denied: type '", resolved.type_name, "' has deny_create = true")));
      return result; // short-circuit
    }
    if (op == WriteOperation::kUpdate && resolved.deny_update) {
      result.violations.push_back(MakeViolation(ArtifactWriteViolation::MUTATION_DENIED, absl::StrCat("type: ", resolved.type_name),
                                                absl::StrCat("UpdateArtifact denied: type '", resolved.type_name, "' has deny_update = true")));
      return result; // short-circuit
    }
  }

  // ── Phase 3: EMPTY_PAYLOAD ────────────────────────────────────────────────
  if (payload.empty()) {
    result.violations.push_back(MakeViolation(ArtifactWriteViolation::EMPTY_PAYLOAD, absl::StrCat("type: ", resolved.type_name),
                                              "empty payload: zero-length payloads are reserved for tombstones"));
    return result; // short-circuit
  }

  // ── Phase 4: PAYLOAD_VALIDATION_FAILURE ───────────────────────────────────
  auto parse_violation = ValidatePayloadParsing(resolved.descriptor_set, resolved.type_name, payload);
  if (parse_violation.has_value()) {
    result.violations.push_back(std::move(*parse_violation));
    return result; // short-circuit
  }

  // Phases 1-4 passed; populate the resolved type for the caller.
  result.resolved_type = std::move(*resolved_or);

  // ── Phase 5: NAN_IN_INDEXED_FIELD, NON_MINIMAL_VARINT ────────────────────
  auto index_violations = ValidateIndexDerivation(result.resolved_type->descriptor_set, result.resolved_type->type_name, payload, index_def_ids_by_key_type);

  // ── Phase 6: Referential integrity (collected, not short-circuited) ───────
  // Referential integrity checking is handled externally by the referential
  // integrity module.  This function does not perform it.

  // Collect all violations from phases 5 and 6.
  result.violations.insert(result.violations.end(), std::make_move_iterator(index_violations.begin()), std::make_move_iterator(index_violations.end()));

  return result;
}

absl::StatusOr<std::vector<ArtifactWriteViolation>> ValidateDelete(uint64_t artifact_id, const ValidationContext& ctx) {
  std::vector<ArtifactWriteViolation> violations;

  // Step 1: Read the existing artifact to get its type_name.
  auto stored_or = ReadStoredArtifact(artifact_id, ctx);
  if (!stored_or.ok()) {
    return stored_or.status();
  }
  const StoredArtifact& stored = *stored_or;

  // Tombstoned artifacts (empty payload) can still be "deleted" idempotently,
  // but we still need the type_name for mutation checks.
  const std::string& type_name = stored.type_name();

  // Step 2: Resolve the TypeDefinition to get deny_delete.
  // We read the TypeVersionDefinition to get type_id, then the TypeDefinition.
  auto resolved_or = ResolveVersionId(stored.version_id(), ctx);
  if (!resolved_or.ok()) {
    // If version resolution fails, we cannot check deny_delete.  This is an
    // internal consistency error rather than a user validation failure.
    return resolved_or.status();
  }
  const ResolvedType& resolved = *resolved_or;

  // Step 3: MUTATION_DENIED — check deny_delete.
  if (!ctx.bypass_mutation_check && resolved.deny_delete) {
    violations.push_back(MakeViolation(ArtifactWriteViolation::MUTATION_DENIED, absl::StrCat("type: ", type_name),
                                       absl::StrCat("DeleteArtifact denied: type '", type_name, "' has deny_delete = true")));
    return violations; // short-circuit
  }

  // Step 4: REFERENCE_DELETE_RESTRICTED is handled externally by the
  // referential integrity module.

  return violations;
}

} // namespace artifact_system::artifact

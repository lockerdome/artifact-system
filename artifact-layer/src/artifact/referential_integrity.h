#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"

#include "artifact_service.pb.h"
#include "storage/storage_interface.h"

namespace artifact_system::artifact {

// Context for referential integrity checks.
struct RefIntegrityContext {
  StorageInterface* storage;
  std::string ref; // branch name or commit ID for reads
};

// Action to perform as a result of delete-time enforcement.
struct CascadeDelete {
  uint64_t artifact_id;
};

struct SetNullUpdate {
  uint64_t referencing_artifact_id;
  std::string field_name;
  uint64_t removed_reference_id; // the ID being removed from the field
};

using DeleteSideEffect = std::variant<CascadeDelete, SetNullUpdate>;

// Write-time: validate reference fields on a parsed message.
// Iterates all fields of the descriptor, checks for the `references` extension
// option, and validates each reference value against storage.
// Returns violations for REFERENCE_TARGET_NOT_FOUND, REFERENCE_TARGET_TOMBSTONED,
// REFERENCE_TARGET_WRONG_TYPE, REFERENCE_DUPLICATE_VALUE.
absl::StatusOr<std::vector<ArtifactWriteViolation>> ValidateReferences(const google::protobuf::Message& message, const google::protobuf::Descriptor& descriptor,
                                                                       const RefIntegrityContext& ctx);

// Delete-time enforcement result.
struct DeleteEnforcementResult {
  std::vector<ArtifactWriteViolation> violations;
  std::vector<DeleteSideEffect> side_effects;
};

// Delete-time: enforce referential integrity for a deleted artifact.
// Uses the `references_by_target_type` index to discover ReferenceDefinitions
// that target the deleted artifact's type, then checks covering indexes to
// find referencing artifacts.
// Returns violations (REFERENCE_DELETE_RESTRICTED for RESTRICT refs) and
// side effects (CascadeDelete, SetNullUpdate for CASCADE/SET_NULL refs).
//
// `scheduled_deletes` prevents infinite recursion for mutually-referencing types.
// `index_def_ids_by_key_type` maps key_type -> IndexDefinition artifact_id.
absl::StatusOr<DeleteEnforcementResult> EnforceDeleteIntegrity(uint64_t artifact_id, const std::string& type_name, const RefIntegrityContext& ctx,
                                                               const std::set<uint64_t>& scheduled_deletes = {},
                                                               const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type = {});

} // namespace artifact_system::artifact

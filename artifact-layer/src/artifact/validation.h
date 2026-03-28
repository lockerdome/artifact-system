#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact_service.pb.h"
#include "storage/storage_interface.h"

namespace artifact_system::artifact {

// The resolved type information needed for validation.
struct ResolvedType {
  uint64_t type_def_id;  // TypeDefinition artifact_id
  std::string type_name; // e.g., "TypeDefinition"
  bool deny_create;
  bool deny_update;
  bool deny_delete;
  uint64_t version_id; // TypeVersionDefinition artifact_id
  google::protobuf::FileDescriptorSet descriptor_set;
};

// Operation being validated.
enum class WriteOperation {
  kCreate,
  kUpdate,
  kDelete,
};

// Context for validation — provides read access to storage.
struct ValidationContext {
  StorageInterface* storage;
  std::string ref; // branch name or commit ID for reads
  // Optional: for bypassing mutation restrictions (P7b internal bypass).
  bool bypass_mutation_check = false;
};

// Resolve a version_id to the full ResolvedType.
// On failure, returns a single-element vector containing an INVALID_VERSION_ID
// violation wrapped in an OK StatusOr.  Returns a non-OK status only for
// unexpected I/O errors that are NOT validation failures.
absl::StatusOr<ResolvedType> ResolveVersionId(uint64_t version_id, const ValidationContext& ctx);

// Result of ValidateCreateOrUpdate: violations + resolved type info (when
// phases 1-4 pass, the resolved type is available for downstream use).
struct ValidationResult {
  std::vector<ArtifactWriteViolation> violations;
  std::optional<ResolvedType> resolved_type; // populated when phases 1-4 pass
};

// Full validation for Create/Update.
// Returns empty violations on success, or collected violations on failure.
// When phases 1-4 pass, resolved_type is populated so the caller can reuse it.
absl::StatusOr<ValidationResult> ValidateCreateOrUpdate(WriteOperation op, uint64_t version_id, const std::string& payload, const ValidationContext& ctx,
                                                        std::optional<uint64_t> existing_artifact_id = std::nullopt,
                                                        const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type = {});

// Validation for Delete (mutation check only; referential integrity is
// handled separately).
absl::StatusOr<std::vector<ArtifactWriteViolation>> ValidateDelete(uint64_t artifact_id, const ValidationContext& ctx);

} // namespace artifact_system::artifact

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "absl/status/statusor.h"
#include "storage/storage_interface.h"

namespace artifact_system::bootstrap {

// Pre-allocated artifact IDs for genesis bootstrap.
// These are fixed constants — the genesis commit uses them directly instead
// of going through the ID allocator service.
struct GenesisIds {
  // IndexDefinition type (created first — all other types declare indexes)
  static constexpr uint64_t kIndexDefinitionTypeDef = 1;
  static constexpr uint64_t kIndexDefinitionTypeVersionDef = 2;

  // Bootstrap IndexDefinition artifacts for IndexDefinition type
  static constexpr uint64_t kIndexKeyTypeUnique = 3;
  static constexpr uint64_t kAllIndexDefinitions = 4;

  // TypeDefinition type
  static constexpr uint64_t kTypeDefinitionTypeDef = 5;
  static constexpr uint64_t kTypeDefinitionTypeVersionDef = 6;

  // Bootstrap IndexDefinition artifacts for TypeDefinition type
  static constexpr uint64_t kTypeNameUnique = 7;
  static constexpr uint64_t kAllTypes = 8;

  // TypeVersionDefinition type
  static constexpr uint64_t kTypeVersionDefinitionTypeDef = 9;
  static constexpr uint64_t kTypeVersionDefinitionTypeVersionDef = 10;

  // Bootstrap IndexDefinition artifact for TypeVersionDefinition type
  static constexpr uint64_t kTypeVersionsByType = 11;

  // ReferenceDefinition type
  static constexpr uint64_t kReferenceDefinitionTypeDef = 12;
  static constexpr uint64_t kReferenceDefinitionTypeVersionDef = 13;

  // Bootstrap IndexDefinition artifacts for ReferenceDefinition type
  static constexpr uint64_t kReferenceKeyTypeUnique = 14;
  static constexpr uint64_t kReferencesByTargetType = 15;
  static constexpr uint64_t kAllReferenceDefinitions = 16;

  // Built-in ReferenceDefinition artifacts
  static constexpr uint64_t kRefTypeVersionDefTypeId = 17;

  // One past the last pre-allocated ID. The ID allocator should start here.
  static constexpr uint64_t kFirstUserAllocatableId = 18;
};

// Result of a successful genesis bootstrap.
struct GenesisResult {
  // Map from index key_type to IndexDefinition artifact_id.
  // Callers use this to initialize TypeRegistry and ArtifactStore.
  std::unordered_map<std::string, uint64_t> index_def_ids_by_key_type;
};

// Run the genesis bootstrap: create all built-in type artifacts, index
// definition artifacts, reference definition artifacts, and their derived
// index entries in a single atomic commit to the canonical branch.
//
// Idempotent: if the canonical branch already contains genesis state,
// returns the existing index mapping without writing anything.
absl::StatusOr<GenesisResult> RunGenesis(StorageInterface* storage);

} // namespace artifact_system::bootstrap

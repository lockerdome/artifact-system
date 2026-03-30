#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact/artifact_store.h"
#include "artifact_options.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "id/id_allocator_interface.h"
#include "registry/internal_bypass.h"
#include "registry/proto_compiler.h"
#include "registry/schema_compatibility.h"
#include "storage/storage_interface.h"
#include "transaction/transaction_manager.h"

namespace artifact_system::registry {

// Result of a successful RegisterTypeVersion call.
struct RegisterResult {
  uint64_t version_id = 0; // TypeVersionDefinition artifact_id
};

// Result of GetTypeVersion.
struct TypeVersionInfo {
  uint64_t version_id = 0;
  uint64_t type_id = 0;
  google::protobuf::FileDescriptorSet descriptor_set;
  std::string proto_source;
  std::optional<uint64_t> previous_version_id;
  std::optional<uint64_t> next_version_id;
};

// Result of GetIndexSchema.
struct IndexSchemaInfo {
  uint64_t index_definition_id = 0;
  std::string key_type;
  std::vector<std::string> key_fields;
  std::vector<OrderDefinition> order_fields;
  bool unique = false;
  google::protobuf::FileDescriptorSet index_descriptor_set;
  std::string key_message_name;
  std::string value_message_name;
  std::string index_message_name;
};

// TypeRegistry orchestrates the full RegisterTypeVersion workflow and provides
// type introspection APIs (GetTypeVersion, ListTypeVersions, GetIndexSchema).
//
// It coordinates ProtoCompiler, SchemaCompatibility, and ArtifactStore (with
// internal bypass of mutation restrictions) to atomically register new type
// versions.
class TypeRegistry {
public:
  TypeRegistry(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator,
               const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type);

  // RegisterTypeVersion: compile proto source, validate, and atomically create
  // all required artifacts (TypeDefinition, TypeVersionDefinition,
  // IndexDefinitions, ReferenceDefinitions) in a single transaction.
  //
  // On validation failure, returns INVALID_ARGUMENT with a
  // RegisterTypeVersionError payload containing all violations.
  absl::StatusOr<RegisterResult> RegisterTypeVersion(const std::string& type_name, const std::string& proto_source,
                                                     std::optional<bool> deny_create = std::nullopt, std::optional<bool> deny_update = std::nullopt,
                                                     std::optional<bool> deny_delete = std::nullopt);

  // GetTypeVersion: return details about a specific TypeVersionDefinition.
  absl::StatusOr<TypeVersionInfo> GetTypeVersion(uint64_t version_id);

  // ListTypeVersions: return version IDs for a type in creation order.
  absl::StatusOr<std::vector<uint64_t>> ListTypeVersions(const std::string& type_name);

  // GetIndexSchema: return index definition and generated proto schema.
  absl::StatusOr<IndexSchemaInfo> GetIndexSchema(const std::string& key_type);

  // Update the index_def_ids_by_key_type map (called after bootstrap or
  // registration to keep the map current).
  void UpdateIndexDefIds(const std::unordered_map<std::string, uint64_t>& new_ids);

private:
  // Build a RegisterTypeVersionError status from violations.
  static absl::Status MakeRegistrationError(const std::vector<TypeRegistrationViolation>& violations);

  // Helper: build a single TypeRegistrationViolation.
  static TypeRegistrationViolation MakeViolation(TypeRegistrationViolation::Category category, const std::string& subject, const std::string& description);

  // Look up a TypeDefinition by type_name using the type_name_unique index.
  // Returns nullopt if not found.
  absl::StatusOr<std::optional<std::pair<uint64_t, TypeDefinition>>> LookupTypeDefinition(const std::string& ref, const std::string& type_name);

  // Find the tail TypeVersionDefinition (next_version_id unset) for a type.
  absl::StatusOr<std::optional<std::pair<uint64_t, TypeVersionDefinition>>> FindTailVersion(const std::string& ref, uint64_t type_def_id);

  // Generic lookup by string key_type on the first index extension of type T.
  template <typename T>
  absl::StatusOr<std::optional<std::pair<uint64_t, T>>> LookupByKeyType(const std::string& ref, const std::string& index_name, const std::string& key_type);

  // Look up an IndexDefinition by key_type using the index_key_type_unique index.
  absl::StatusOr<std::optional<std::pair<uint64_t, IndexDefinition>>> LookupIndexDefinition(const std::string& ref, const std::string& key_type);

  // Look up a ReferenceDefinition by key_type using the reference_key_type_unique index.
  absl::StatusOr<std::optional<std::pair<uint64_t, ReferenceDefinition>>> LookupReferenceDefinition(const std::string& ref, const std::string& key_type);

  // Read artifact IDs from an index (returns all artifact_ids in the index object).
  absl::StatusOr<std::vector<uint64_t>> ReadIndexArtifactIds(const std::string& ref, uint64_t index_def_id, const std::vector<uint8_t>& encoded_key,
                                                             const IndexDefinition& index_def, const google::protobuf::Descriptor& parent_descriptor);

  // Read TypeVersionDefinition artifact IDs for a type via type_versions_by_type index.
  absl::StatusOr<std::vector<uint64_t>> ReadVersionIdsByType(const std::string& ref, uint64_t type_def_id);

  // Rebuild bypass_store_ with current index_def_ids_by_key_type_.
  void RebuildBypassStore();

  StorageInterface* storage_;
  transaction::TransactionManager* transaction_manager_;
  IdAllocatorInterface* id_allocator_;
  std::unordered_map<std::string, uint64_t> index_def_ids_by_key_type_;
  ProtoCompiler compiler_;
  InternalBypassToken bypass_token_;

  // Internal ArtifactStore with mutation bypass for registration operations.
  std::unique_ptr<artifact::ArtifactStore> bypass_store_;
};

} // namespace artifact_system::registry

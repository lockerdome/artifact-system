#include "registry/type_registry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact/artifact_store.h"
#include "artifact/proto_utils.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "index/index_utils.h"
#include "transaction/context_ref_resolver.h"

namespace artifact_system::registry {
namespace {

// Parse a typed artifact payload from storage.
template <typename T> absl::StatusOr<T> ParseArtifactPayload(StorageInterface* storage, const std::string& ref, uint64_t artifact_id) {
  auto stored_or = artifact::ReadStoredArtifact(storage, ref, artifact_id);
  if (!stored_or.ok())
    return stored_or.status();
  if (stored_or->payload().empty())
    return absl::NotFoundError(absl::StrCat("artifact ", artifact_id, " is tombstoned"));
  T result;
  if (!result.ParseFromString(stored_or->payload()))
    return absl::InternalError(absl::StrCat("failed to parse payload for artifact ", artifact_id));
  return result;
}

// Extract IndexDefinitions from a message descriptor's custom options.
std::vector<IndexDefinition> ExtractIndexDefinitions(const google::protobuf::Descriptor& descriptor) {
  std::vector<IndexDefinition> result;
  const auto& options = descriptor.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    result.push_back(options.GetExtension(artifact_system::indexes, i));
  }
  return result;
}

// Extract reference declarations from a message descriptor's field options.
struct ExtractedReference {
  std::string field_name;
  ReferenceOption option;
  const google::protobuf::FieldDescriptor* field_descriptor;
};

std::vector<ExtractedReference> ExtractReferenceDeclarations(const google::protobuf::Descriptor& descriptor) {
  std::vector<ExtractedReference> result;
  for (int i = 0; i < descriptor.field_count(); ++i) {
    const auto* field = descriptor.field(i);
    if (field->options().HasExtension(artifact_system::references)) {
      const auto& ref_opt = field->options().GetExtension(artifact_system::references);
      result.push_back({std::string(field->name()), ref_opt, field});
    }
  }
  return result;
}

// Validate a new index definition structurally.
std::vector<TypeRegistrationViolation> ValidateNewIndexDefinition(const IndexDefinition& def, const google::protobuf::Descriptor& descriptor) {
  std::vector<TypeRegistrationViolation> violations;

  // Check ORDER_BY_UNSPECIFIED on order fields.
  for (const auto& order : def.order()) {
    if (order.direction() == OrderDefinition::ORDER_BY_UNSPECIFIED) {
      TypeRegistrationViolation v;
      v.set_category(TypeRegistrationViolation::INVALID_INDEX_DEFINITION);
      v.set_subject(absl::StrCat("index: ", def.key_type()));
      v.set_description(absl::StrCat("order field '", order.field(), "' has ORDER_BY_UNSPECIFIED direction"));
      violations.push_back(std::move(v));
    }
  }

  // Check for more than one repeated field across key and order fields.
  int repeated_count = 0;
  for (const auto& key_field_name : def.key()) {
    const auto* field = descriptor.FindFieldByName(key_field_name);
    if (field != nullptr && field->is_repeated()) {
      ++repeated_count;
    }
  }
  for (const auto& order : def.order()) {
    if (order.field() == "artifact_id")
      continue;
    const auto* field = descriptor.FindFieldByName(order.field());
    if (field != nullptr && field->is_repeated()) {
      ++repeated_count;
    }
  }
  if (repeated_count > 1) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_INDEX_DEFINITION);
    v.set_subject(absl::StrCat("index: ", def.key_type()));
    v.set_description("more than one repeated field referenced across key and order fields");
    violations.push_back(std::move(v));
  }

  return violations;
}

// Check if two IndexDefinitions are compatible (no modifications to key/order/unique).
std::vector<TypeRegistrationViolation> CheckIndexCompatibility(const IndexDefinition& existing, const IndexDefinition& proposed) {
  std::vector<TypeRegistrationViolation> violations;
  bool incompatible = false;

  // Check key fields.
  if (existing.key_size() != proposed.key_size()) {
    incompatible = true;
  } else {
    for (int i = 0; i < existing.key_size(); ++i) {
      if (existing.key(i) != proposed.key(i)) {
        incompatible = true;
        break;
      }
    }
  }

  // Check order fields.
  if (!incompatible && existing.order_size() != proposed.order_size()) {
    incompatible = true;
  } else if (!incompatible) {
    for (int i = 0; i < existing.order_size(); ++i) {
      if (existing.order(i).field() != proposed.order(i).field() || existing.order(i).direction() != proposed.order(i).direction()) {
        incompatible = true;
        break;
      }
    }
  }

  // Check unique flag.
  if (!incompatible && existing.unique() != proposed.unique()) {
    incompatible = true;
  }

  if (incompatible) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INDEX_INCOMPATIBILITY);
    v.set_subject(absl::StrCat("index: ", proposed.key_type()));
    v.set_description(absl::StrCat("existing index definition '", proposed.key_type(), "' was modified (key fields, order fields, or unique flag changed)"));
    violations.push_back(std::move(v));
  }
  return violations;
}

// Validate a reference declaration structurally.
std::vector<TypeRegistrationViolation> ValidateNewReferenceDeclaration(const ExtractedReference& ref, const google::protobuf::Descriptor& descriptor,
                                                                       const std::vector<IndexDefinition>& index_defs,
                                                                       const std::function<bool(const std::string&)>& type_exists) {
  std::vector<TypeRegistrationViolation> violations;
  const std::string subject = absl::StrCat("reference: ", descriptor.full_name(), ".", ref.field_name);

  // Check field type is uint64, optional uint64, or repeated uint64.
  const auto* field = ref.field_descriptor;
  if (field->type() != google::protobuf::FieldDescriptor::TYPE_UINT64) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description(absl::StrCat("references option on non-uint64 field '", ref.field_name, "'"));
    violations.push_back(std::move(v));
    return violations;
  }

  // Check ON_DELETE_UNSPECIFIED.
  if (ref.option.on_delete() == ReferenceOption::ON_DELETE_UNSPECIFIED) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description("on_delete is ON_DELETE_UNSPECIFIED");
    violations.push_back(std::move(v));
  }

  // Check SET_NULL on implicit-presence scalar.
  if (ref.option.on_delete() == ReferenceOption::SET_NULL && !field->is_repeated() && !field->has_presence()) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description("SET_NULL on implicit-presence scalar field");
    violations.push_back(std::move(v));
  }

  // Check target_type_name resolves.
  if (!type_exists(ref.option.target_type_name())) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description(absl::StrCat("target_type_name '", ref.option.target_type_name(), "' does not resolve to an existing TypeDefinition"));
    violations.push_back(std::move(v));
  }

  // Check covering index: exactly one index where the reference field is the sole key.
  int covering_count = 0;
  for (const auto& idx : index_defs) {
    if (idx.key_size() == 1 && idx.key(0) == ref.field_name) {
      ++covering_count;
    }
  }
  if (covering_count == 0) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description(absl::StrCat("no covering index with '", ref.field_name, "' as sole key"));
    violations.push_back(std::move(v));
  } else if (covering_count > 1) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::INVALID_REFERENCE_DECLARATION);
    v.set_subject(subject);
    v.set_description(absl::StrCat("ambiguous: multiple indexes with '", ref.field_name, "' as sole key"));
    violations.push_back(std::move(v));
  }

  return violations;
}

// Check if two ReferenceDefinitions are compatible.
std::vector<TypeRegistrationViolation> CheckReferenceCompatibility(const ReferenceDefinition& existing, const ReferenceDefinition& proposed) {
  std::vector<TypeRegistrationViolation> violations;
  bool incompatible = false;

  if (existing.target_type_name() != proposed.target_type_name())
    incompatible = true;
  if (existing.field_name() != proposed.field_name())
    incompatible = true;
  if (existing.covering_index_key_type() != proposed.covering_index_key_type())
    incompatible = true;
  if (existing.on_delete() != proposed.on_delete())
    incompatible = true;

  if (incompatible) {
    TypeRegistrationViolation v;
    v.set_category(TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY);
    v.set_subject(absl::StrCat("reference: ", proposed.key_type()));
    v.set_description(absl::StrCat("existing reference definition '", proposed.key_type(), "' was modified"));
    violations.push_back(std::move(v));
  }
  return violations;
}

// Determine the covering index key_type for a reference field.
std::string FindCoveringIndexKeyType(const std::string& field_name, const std::vector<IndexDefinition>& index_defs) {
  for (const auto& idx : index_defs) {
    if (idx.key_size() == 1 && idx.key(0) == field_name) {
      return idx.key_type();
    }
  }
  return "";
}

// Check for items in `old_items` that are missing from `new_items`.
// `get_key` extracts the comparison key from each item.
// `make_violation` builds the violation for each missing item.
template <typename T, typename KeyFn, typename ViolationFn>
void CheckForRemovals(const std::vector<T>& old_items, const std::vector<T>& new_items, KeyFn get_key, ViolationFn make_violation,
                      std::vector<TypeRegistrationViolation>& violations) {
  for (const auto& old_item : old_items) {
    const auto old_key = get_key(old_item);
    bool found = false;
    for (const auto& new_item : new_items) {
      if (get_key(new_item) == old_key) {
        found = true;
        break;
      }
    }
    if (!found) {
      violations.push_back(make_violation(old_item));
    }
  }
}

// Aliases for encoding convenience functions.
const auto& EncodeStringIndexKey = encoding::EncodeSingleStringKey;
const auto& EncodeUint64IndexKey = encoding::EncodeSingleUint64Key;

// Populate an IndexSchemaInfo from an IndexDefinition and generated schema.
IndexSchemaInfo BuildIndexSchemaInfo(uint64_t idx_id, const IndexDefinition& idx_def, const index::GeneratedIndexSchema& schema) {
  IndexSchemaInfo info;
  info.index_definition_id = idx_id;
  info.key_type = idx_def.key_type();
  for (const auto& k : idx_def.key())
    info.key_fields.push_back(k);
  for (const auto& o : idx_def.order())
    info.order_fields.push_back(o);
  info.unique = idx_def.unique();

  google::protobuf::FileDescriptorSet gen_fds;
  // Include transitive dependencies so the descriptor set is self-contained.
  std::set<const google::protobuf::FileDescriptor*> seen;
  std::function<void(const google::protobuf::FileDescriptor*)> add_file;
  add_file = [&](const google::protobuf::FileDescriptor* fd) {
    if (!seen.insert(fd).second)
      return;
    for (int i = 0; i < fd->dependency_count(); ++i)
      add_file(fd->dependency(i));
    fd->CopyTo(gen_fds.add_file());
  };
  add_file(schema.file_descriptor);
  info.index_descriptor_set = gen_fds;

  info.key_message_name = schema.key_descriptor->full_name();
  info.value_message_name = schema.value_descriptor->full_name();
  info.index_message_name = schema.index_descriptor->full_name();
  return info;
}

} // namespace

TypeRegistry::TypeRegistry(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator,
                           const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type)
    : storage_(storage), transaction_manager_(transaction_manager), id_allocator_(id_allocator), index_def_ids_by_key_type_(index_def_ids_by_key_type),
      bypass_token_() {
  artifact::ArtifactStore::Options opts;
  opts.index_def_ids_by_key_type = &index_def_ids_by_key_type_;
  opts.bypass_mutation_check = true;
  opts.bypass_referential_integrity = true;
  bypass_store_ = std::make_unique<artifact::ArtifactStore>(storage_, transaction_manager_, id_allocator_, opts);
}

// static
absl::Status TypeRegistry::MakeRegistrationError(const std::vector<TypeRegistrationViolation>& violations) {
  RegisterTypeVersionError error;
  for (const auto& v : violations) {
    *error.add_violations() = v;
  }
  absl::Status status = absl::InvalidArgumentError("type registration validation failed");
  std::string serialized;
  error.SerializeToString(&serialized);
  status.SetPayload("type.googleapis.com/artifact_system.RegisterTypeVersionError", absl::Cord(serialized));
  return status;
}

// static
TypeRegistrationViolation TypeRegistry::MakeViolation(TypeRegistrationViolation::Category category, const std::string& subject,
                                                      const std::string& description) {
  TypeRegistrationViolation v;
  v.set_category(category);
  v.set_subject(subject);
  v.set_description(description);
  return v;
}

absl::StatusOr<uint64_t> TypeRegistry::ResolveIndexDefId(const std::string& key_type) {
  auto id_or = index::ResolveIndexDefinitionId(storage_, index_def_ids_by_key_type_, key_type);
  if (!id_or.ok()) {
    return id_or.status();
  }
  index_def_ids_by_key_type_[key_type] = *id_or;
  return *id_or;
}

absl::StatusOr<std::optional<std::pair<uint64_t, TypeDefinition>>> TypeRegistry::LookupTypeDefinition(const std::string& ref, const std::string& type_name) {
  auto id_or = ResolveIndexDefId("type_name_unique");
  if (!id_or.ok())
    return id_or.status();

  const auto* td_descriptor = TypeDefinition::descriptor();
  auto encoded_or = EncodeStringIndexKey(*td_descriptor, "type_name", type_name);
  if (!encoded_or.ok())
    return encoded_or.status();

  const IndexDefinition& idx_def = td_descriptor->options().GetExtension(artifact_system::indexes, 0); // type_name_unique is first
  auto ids_or = ReadIndexArtifactIds(ref, *id_or, *encoded_or, idx_def, *td_descriptor);
  if (!ids_or.ok()) {
    if (absl::IsNotFound(ids_or.status()))
      return std::nullopt;
    return ids_or.status();
  }
  if (ids_or->empty())
    return std::nullopt;

  auto td_or = ParseArtifactPayload<TypeDefinition>(storage_, ref, ids_or->front());
  if (!td_or.ok())
    return td_or.status();
  return std::make_pair(ids_or->front(), std::move(*td_or));
}

absl::StatusOr<std::optional<std::pair<uint64_t, TypeVersionDefinition>>> TypeRegistry::FindTailVersion(const std::string& ref, uint64_t type_def_id) {
  auto ids_or = ReadVersionIdsByType(ref, type_def_id);
  if (!ids_or.ok()) {
    if (absl::IsNotFound(ids_or.status()))
      return std::nullopt;
    return ids_or.status();
  }
  if (ids_or->empty())
    return std::nullopt;

  // The tail is the version whose next_version_id is unset.
  // Iterate from the end (most recently created) to find it faster.
  for (auto rit = ids_or->rbegin(); rit != ids_or->rend(); ++rit) {
    auto tvd_or = ParseArtifactPayload<TypeVersionDefinition>(storage_, ref, *rit);
    if (!tvd_or.ok())
      continue;
    if (!tvd_or->has_next_version_id()) {
      return std::make_pair(*rit, std::move(*tvd_or));
    }
  }
  return std::nullopt;
}

// Generic helper: look up an artifact by string key_type on the first index extension.
// Returns nullopt if not found. T must be a protobuf message type with a "key_type" field
// and the first index extension being the unique lookup index.
template <typename T>
absl::StatusOr<std::optional<std::pair<uint64_t, T>>> TypeRegistry::LookupByKeyType(const std::string& ref, const std::string& index_name,
                                                                                    const std::string& key_type) {
  auto id_or = ResolveIndexDefId(index_name);
  if (!id_or.ok())
    return id_or.status();

  const auto* descriptor = T::descriptor();
  auto encoded_or = EncodeStringIndexKey(*descriptor, "key_type", key_type);
  if (!encoded_or.ok())
    return encoded_or.status();

  const IndexDefinition& idx_def = descriptor->options().GetExtension(artifact_system::indexes, 0);
  auto ids_or = ReadIndexArtifactIds(ref, *id_or, *encoded_or, idx_def, *descriptor);
  if (!ids_or.ok()) {
    if (absl::IsNotFound(ids_or.status()))
      return std::nullopt;
    return ids_or.status();
  }
  if (ids_or->empty())
    return std::nullopt;

  auto payload_or = ParseArtifactPayload<T>(storage_, ref, ids_or->front());
  if (!payload_or.ok())
    return payload_or.status();
  return std::make_pair(ids_or->front(), std::move(*payload_or));
}

absl::StatusOr<std::optional<std::pair<uint64_t, IndexDefinition>>> TypeRegistry::LookupIndexDefinition(const std::string& ref, const std::string& key_type) {
  return LookupByKeyType<IndexDefinition>(ref, "index_key_type_unique", key_type);
}

absl::StatusOr<std::optional<std::pair<uint64_t, ReferenceDefinition>>> TypeRegistry::LookupReferenceDefinition(const std::string& ref,
                                                                                                                const std::string& key_type) {
  return LookupByKeyType<ReferenceDefinition>(ref, "reference_key_type_unique", key_type);
}

absl::StatusOr<std::vector<uint64_t>> TypeRegistry::ReadVersionIdsByType(const std::string& ref, uint64_t type_def_id) {
  auto id_or = ResolveIndexDefId("type_versions_by_type");
  if (!id_or.ok())
    return id_or.status();

  const auto* tvd_descriptor = TypeVersionDefinition::descriptor();
  auto encoded_or = EncodeUint64IndexKey(*tvd_descriptor, "type_id", type_def_id);
  if (!encoded_or.ok())
    return encoded_or.status();

  const IndexDefinition& idx_def = tvd_descriptor->options().GetExtension(artifact_system::indexes, 0);
  return ReadIndexArtifactIds(ref, *id_or, *encoded_or, idx_def, *tvd_descriptor);
}

absl::StatusOr<std::vector<uint64_t>> TypeRegistry::ReadIndexArtifactIds(const std::string& ref, uint64_t index_def_id, const std::vector<uint8_t>& encoded_key,
                                                                         const IndexDefinition& index_def,
                                                                         const google::protobuf::Descriptor& parent_descriptor) {
  const std::string index_path = encoding::IndexPath(index_def_id, encoded_key);
  auto data_or = storage_->GetObject(ref, index_path);
  if (!data_or.ok())
    return data_or.status();

  auto schema_or = index::GenerateIndexSchema(index_def, parent_descriptor);
  if (!schema_or.ok())
    return schema_or.status();

  auto obj_or = index::DeserializeIndexObject(*schema_or, index_def, *data_or);
  if (!obj_or.ok())
    return obj_or.status();

  std::vector<uint64_t> ids;
  ids.reserve(obj_or->rows.size());
  for (const auto& row : obj_or->rows) {
    ids.push_back(row.artifact_id);
  }
  return ids;
}

// ── RegisterTypeVersion ────────────────────────────────────────────────────

absl::StatusOr<RegisterResult> TypeRegistry::RegisterTypeVersion(const std::string& type_name, const std::string& proto_source, std::optional<bool> deny_create,
                                                                 std::optional<bool> deny_update, std::optional<bool> deny_delete,
                                                                 std::optional<std::string> transaction_id) {
  // Step 0: Compile .proto source.
  auto compile_result = compiler_.Compile(proto_source, type_name);
  if (std::holds_alternative<CompilationError>(compile_result)) {
    auto& err = std::get<CompilationError>(compile_result);
    return MakeRegistrationError({MakeViolation(TypeRegistrationViolation::PROTO_COMPILATION_FAILURE, absl::StrCat("type: ", type_name), err.description)});
  }
  auto& compilation = std::get<CompilationResult>(compile_result);
  const auto& new_descriptor_set = compilation.descriptor_set;

  // Build a pool to find the message descriptor in the compiled result.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* new_descriptor = artifact::BuildPoolAndFindMessage(new_descriptor_set, type_name, &pool);
  if (new_descriptor == nullptr) {
    return MakeRegistrationError({MakeViolation(TypeRegistrationViolation::PROTO_COMPILATION_FAILURE, absl::StrCat("type: ", type_name),
                                                absl::StrCat("message '", type_name, "' not found in compiled descriptor set"))});
  }

  // Collect all non-fatal violations.
  std::vector<TypeRegistrationViolation> violations;

  auto ref_or = transaction::ResolveWriteRef(storage_, transaction_manager_, transaction_id);
  if (!ref_or.ok()) {
    return ref_or.status();
  }
  const std::string ref = *ref_or;

  // Step 1: Look up TypeDefinition.
  auto td_or = LookupTypeDefinition(ref, type_name);
  if (!td_or.ok())
    return td_or.status();

  std::optional<uint64_t> existing_td_id;
  std::optional<TypeDefinition> existing_td;
  if (td_or->has_value()) {
    existing_td_id = (*td_or)->first;
    existing_td = (*td_or)->second;

    // Tighten-only enforcement for deny flags.
    if (deny_create.has_value() && !*deny_create && existing_td->deny_create()) {
      violations.push_back(
          MakeViolation(TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION, "flag: deny_create", "deny_create cannot be changed from true to false"));
    }
    if (deny_update.has_value() && !*deny_update && existing_td->deny_update()) {
      violations.push_back(
          MakeViolation(TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION, "flag: deny_update", "deny_update cannot be changed from true to false"));
    }
    if (deny_delete.has_value() && !*deny_delete && existing_td->deny_delete()) {
      violations.push_back(
          MakeViolation(TypeRegistrationViolation::TIGHTEN_ONLY_VIOLATION, "flag: deny_delete", "deny_delete cannot be changed from true to false"));
    }
  }

  // Step 2: Schema compatibility check against tail version.
  std::optional<uint64_t> tail_version_id;
  std::optional<TypeVersionDefinition> tail_tvd;
  if (existing_td_id.has_value()) {
    auto tail_or = FindTailVersion(ref, *existing_td_id);
    if (!tail_or.ok())
      return tail_or.status();
    if (tail_or->has_value()) {
      tail_version_id = (*tail_or)->first;
      tail_tvd = (*tail_or)->second;

      auto schema_violations = CheckSchemaCompatibility(tail_tvd->descriptor_set(), new_descriptor_set, type_name);
      for (const auto& sv : schema_violations) {
        violations.push_back(MakeViolation(TypeRegistrationViolation::SCHEMA_INCOMPATIBILITY, sv.subject, sv.description));
      }
    }
  }

  // Step 3: Extract and validate index definitions.
  auto new_index_defs = ExtractIndexDefinitions(*new_descriptor);
  for (const auto& idx_def : new_index_defs) {
    auto existing_idx_or = LookupIndexDefinition(ref, idx_def.key_type());
    if (!existing_idx_or.ok())
      return existing_idx_or.status();

    if (existing_idx_or->has_value()) {
      // Existing index: check compatibility.
      auto compat_violations = CheckIndexCompatibility((*existing_idx_or)->second, idx_def);
      violations.insert(violations.end(), compat_violations.begin(), compat_violations.end());
    } else {
      // New index: validate structurally.
      auto struct_violations = ValidateNewIndexDefinition(idx_def, *new_descriptor);
      violations.insert(violations.end(), struct_violations.begin(), struct_violations.end());
    }
  }

  // Step 4: Extract and validate reference declarations.
  auto new_refs = ExtractReferenceDeclarations(*new_descriptor);
  auto type_exists = [&](const std::string& name) -> bool {
    auto result = LookupTypeDefinition(ref, name);
    return result.ok() && result->has_value();
  };
  for (const auto& ref_decl : new_refs) {
    const std::string ref_key_type = absl::StrCat(type_name, ".", ref_decl.field_name);
    auto existing_ref_or = LookupReferenceDefinition(ref, ref_key_type);
    if (!existing_ref_or.ok())
      return existing_ref_or.status();

    if (existing_ref_or->has_value()) {
      // Existing reference: check compatibility.
      ReferenceDefinition proposed;
      proposed.set_key_type(ref_key_type);
      proposed.set_target_type_name(ref_decl.option.target_type_name());
      proposed.set_referencing_type_name(type_name);
      proposed.set_field_name(ref_decl.field_name);
      proposed.set_covering_index_key_type(FindCoveringIndexKeyType(ref_decl.field_name, new_index_defs));
      proposed.set_on_delete(ref_decl.option.on_delete());
      auto compat_violations = CheckReferenceCompatibility((*existing_ref_or)->second, proposed);
      violations.insert(violations.end(), compat_violations.begin(), compat_violations.end());
    } else {
      // New reference: validate structurally.
      auto struct_violations = ValidateNewReferenceDeclaration(ref_decl, *new_descriptor, new_index_defs, type_exists);
      violations.insert(violations.end(), struct_violations.begin(), struct_violations.end());
    }
  }

  // Check for removed indexes and references against the previous version.
  if (tail_tvd.has_value()) {
    google::protobuf::DescriptorPool old_pool(google::protobuf::DescriptorPool::generated_pool());
    const auto* old_descriptor = artifact::BuildPoolAndFindMessage(tail_tvd->descriptor_set(), type_name, &old_pool);
    if (old_descriptor != nullptr) {
      CheckForRemovals(
          ExtractIndexDefinitions(*old_descriptor), new_index_defs, [](const IndexDefinition& idx) { return idx.key_type(); },
          [](const IndexDefinition& idx) {
            return MakeViolation(TypeRegistrationViolation::INDEX_INCOMPATIBILITY, absl::StrCat("index: ", idx.key_type()),
                                 absl::StrCat("existing index definition '", idx.key_type(), "' was removed"));
          },
          violations);

      CheckForRemovals(
          ExtractReferenceDeclarations(*old_descriptor), new_refs, [&](const ExtractedReference& r) { return absl::StrCat(type_name, ".", r.field_name); },
          [&](const ExtractedReference& r) {
            const std::string key = absl::StrCat(type_name, ".", r.field_name);
            return MakeViolation(TypeRegistrationViolation::REFERENCE_INCOMPATIBILITY, absl::StrCat("reference: ", key),
                                 absl::StrCat("existing reference declaration '", key, "' was removed"));
          },
          violations);
    }
  }

  // If any violations, return them all.
  if (!violations.empty())
    return MakeRegistrationError(violations);

  // ── All validation passed. Now create artifacts in a transaction. ──

  // We need the version_id for the TypeVersionDefinition we're about to create.
  // All these artifacts get IDs allocated before we start.
  // We use an explicit transaction so all writes are atomic.

  auto txn_or = transaction_manager_->CreateTransaction(std::nullopt, transaction_id);
  if (!txn_or.ok())
    return txn_or.status();
  const std::string& txn_id = *txn_or;

  // Scope guard: rollback on any early return (disarmed on successful commit).
  bool committed = false;
  auto rollback_guard = [&] {
    if (!committed)
      (void)transaction_manager_->RollbackTransaction(txn_id);
  };
  struct ScopeGuard {
    std::function<void()> fn;
    ~ScopeGuard() {
      fn();
    }
  } guard{rollback_guard};

  // Look up the current version_id for each built-in meta-type.
  auto resolve_builtin_version = [&](const std::string& builtin_type_name) -> absl::StatusOr<uint64_t> {
    auto type_or = LookupTypeDefinition(ref, builtin_type_name);
    if (!type_or.ok())
      return type_or.status();
    if (!type_or->has_value())
      return absl::InternalError(absl::StrCat(builtin_type_name, " type not found in registry"));
    return (*type_or)->second.current_version_id();
  };

  auto tvd_ver_or = resolve_builtin_version("artifact_system.TypeVersionDefinition");
  if (!tvd_ver_or.ok())
    return tvd_ver_or.status();
  uint64_t tvd_version_id = *tvd_ver_or;

  auto td_ver_or = resolve_builtin_version("artifact_system.TypeDefinition");
  if (!td_ver_or.ok())
    return td_ver_or.status();
  uint64_t td_version_id = *td_ver_or;

  auto idx_ver_or = resolve_builtin_version("artifact_system.IndexDefinition");
  if (!idx_ver_or.ok())
    return idx_ver_or.status();
  uint64_t idx_version_id = *idx_ver_or;

  auto ref_ver_or = resolve_builtin_version("artifact_system.ReferenceDefinition");
  if (!ref_ver_or.ok())
    return ref_ver_or.status();
  uint64_t refdef_version_id = *ref_ver_or;

  // Step 3 (creation): Create IndexDefinition artifacts for new indexes.
  std::unordered_map<std::string, uint64_t> new_index_ids;
  for (const auto& idx_def : new_index_defs) {
    auto existing_idx_or = LookupIndexDefinition(ref, idx_def.key_type());
    if (!existing_idx_or.ok())
      return existing_idx_or.status();

    if (existing_idx_or->has_value()) {
      // Already exists — record its ID for reference.
      new_index_ids[idx_def.key_type()] = (*existing_idx_or)->first;
      continue;
    }

    // Create new IndexDefinition artifact.
    auto create_or = bypass_store_->CreateArtifact(idx_version_id, idx_def.SerializeAsString(), txn_id);
    if (!create_or.ok()) {
      return create_or.status();
    }
    new_index_ids[idx_def.key_type()] = create_or->artifact_id;
  }

  // Step 4 (creation): Create ReferenceDefinition artifacts for new references.
  for (const auto& ref_decl : new_refs) {
    const std::string ref_key_type = absl::StrCat(type_name, ".", ref_decl.field_name);
    auto existing_ref_or = LookupReferenceDefinition(ref, ref_key_type);
    if (!existing_ref_or.ok())
      return existing_ref_or.status();

    if (existing_ref_or->has_value())
      continue; // Already exists.

    ReferenceDefinition rd;
    rd.set_key_type(ref_key_type);
    rd.set_target_type_name(ref_decl.option.target_type_name());
    rd.set_referencing_type_name(type_name);
    rd.set_field_name(ref_decl.field_name);
    rd.set_covering_index_key_type(FindCoveringIndexKeyType(ref_decl.field_name, new_index_defs));
    rd.set_on_delete(ref_decl.option.on_delete());

    auto create_or = bypass_store_->CreateArtifact(refdef_version_id, rd.SerializeAsString(), txn_id);
    if (!create_or.ok()) {
      return create_or.status();
    }
  }

  // Step 1 (creation) + Step 5: Create artifacts in order.
  // For new types: create TD first (with placeholder current_version_id=0),
  // then create TVD, then update TD with the real current_version_id.
  // For existing types: create TVD, update tail, update TD.
  // All writes go through bypass_store_ (which uses WriteExecutor) to avoid
  // leaving uncommitted staging changes on the transaction branch.

  uint64_t type_def_id;
  if (!existing_td_id.has_value()) {
    // Create new TypeDefinition with placeholder current_version_id.
    TypeDefinition new_td;
    new_td.set_type_name(type_name);
    // current_version_id will be updated after TVD creation.
    new_td.set_deny_create(deny_create.value_or(false));
    new_td.set_deny_update(deny_update.value_or(false));
    new_td.set_deny_delete(deny_delete.value_or(false));

    auto create_or = bypass_store_->CreateArtifact(td_version_id, new_td.SerializeAsString(), txn_id);
    if (!create_or.ok()) {
      return create_or.status();
    }
    type_def_id = create_or->artifact_id;
  } else {
    type_def_id = *existing_td_id;
  }

  // Create TypeVersionDefinition via bypass_store_.
  TypeVersionDefinition new_tvd;
  new_tvd.set_type_id(type_def_id);
  *new_tvd.mutable_descriptor_set() = new_descriptor_set;
  new_tvd.set_proto_source(proto_source);
  if (tail_version_id.has_value()) {
    new_tvd.set_previous_version_id(*tail_version_id);
  }

  auto tvd_create_or = bypass_store_->CreateArtifact(tvd_version_id, new_tvd.SerializeAsString(), txn_id);
  if (!tvd_create_or.ok())
    return tvd_create_or.status();
  const uint64_t new_tvd_artifact_id = tvd_create_or->artifact_id;

  // Step 5 continued: Update tail version's next_version_id.
  if (tail_version_id.has_value()) {
    TypeVersionDefinition updated_tail = *tail_tvd;
    updated_tail.set_next_version_id(new_tvd_artifact_id);

    auto update_or = bypass_store_->UpdateArtifact(*tail_version_id, tvd_version_id, updated_tail.SerializeAsString(), txn_id);
    if (!update_or.ok())
      return update_or.status();
  }

  // Step 6: Update TypeDefinition with current_version_id.
  {
    TypeDefinition updated_td;
    if (existing_td.has_value()) {
      updated_td = *existing_td;
    } else {
      auto td_read_or = ParseArtifactPayload<TypeDefinition>(storage_, txn_id, type_def_id);
      if (!td_read_or.ok())
        return td_read_or.status();
      updated_td = *td_read_or;
    }
    updated_td.set_current_version_id(new_tvd_artifact_id);
    if (deny_create.has_value())
      updated_td.set_deny_create(*deny_create);
    if (deny_update.has_value())
      updated_td.set_deny_update(*deny_update);
    if (deny_delete.has_value())
      updated_td.set_deny_delete(*deny_delete);

    auto update_or = bypass_store_->UpdateArtifact(type_def_id, td_version_id, updated_td.SerializeAsString(), txn_id);
    if (!update_or.ok())
      return update_or.status();
  }

  // Step 7: Commit the transaction.
  auto commit_or = transaction_manager_->CommitTransaction(txn_id);
  if (!commit_or.ok())
    return commit_or.status();

  committed = true;

  if (std::holds_alternative<transaction::TransactionManager::CommitConflict>(*commit_or)) {
    return absl::AbortedError("concurrent type registration conflict");
  }

  // Update the index map with newly created index IDs.
  for (const auto& [key, id] : new_index_ids) {
    index_def_ids_by_key_type_[key] = id;
  }

  return RegisterResult{new_tvd_artifact_id};
}

// ── Introspection APIs ─────────────────────────────────────────────────────

absl::StatusOr<TypeVersionInfo> TypeRegistry::GetTypeVersion(uint64_t version_id, const ReadContext& read_context) {
  auto ref_or = transaction::ResolveReadRef(storage_, transaction_manager_, read_context);
  if (!ref_or.ok()) {
    return ref_or.status();
  }
  const std::string ref = *ref_or;
  auto tvd_or = ParseArtifactPayload<TypeVersionDefinition>(storage_, ref, version_id);
  if (!tvd_or.ok())
    return tvd_or.status();

  TypeVersionInfo info;
  info.version_id = version_id;
  info.type_id = tvd_or->type_id();
  info.descriptor_set = tvd_or->descriptor_set();
  info.proto_source = tvd_or->proto_source();
  if (tvd_or->has_previous_version_id())
    info.previous_version_id = tvd_or->previous_version_id();
  if (tvd_or->has_next_version_id())
    info.next_version_id = tvd_or->next_version_id();
  return info;
}

absl::StatusOr<std::vector<uint64_t>> TypeRegistry::ListTypeVersions(const std::string& type_name, const ReadContext& read_context) {
  auto ref_or = transaction::ResolveReadRef(storage_, transaction_manager_, read_context);
  if (!ref_or.ok()) {
    return ref_or.status();
  }
  const std::string ref = *ref_or;

  auto td_or = LookupTypeDefinition(ref, type_name);
  if (!td_or.ok())
    return td_or.status();
  if (!td_or->has_value())
    return absl::NotFoundError(absl::StrCat("type '", type_name, "' not found"));

  auto ids_or = ReadVersionIdsByType(ref, (*td_or)->first);
  if (!ids_or.ok()) {
    if (absl::IsNotFound(ids_or.status()))
      return std::vector<uint64_t>{};
    return ids_or.status();
  }
  return *ids_or;
}

absl::StatusOr<IndexSchemaInfo> TypeRegistry::GetIndexSchema(const std::string& key_type, const ReadContext& read_context) {
  auto ref_or = transaction::ResolveReadRef(storage_, transaction_manager_, read_context);
  if (!ref_or.ok()) {
    return ref_or.status();
  }
  const std::string ref = *ref_or;

  auto idx_or = LookupIndexDefinition(ref, key_type);
  if (!idx_or.ok())
    return idx_or.status();
  if (!idx_or->has_value())
    return absl::NotFoundError(absl::StrCat("index '", key_type, "' not found"));

  const auto& [idx_id, idx_def] = **idx_or;

  // To generate the index schema, we need the parent message descriptor.
  // We need to figure out which type this index belongs to by searching
  // all types. For efficiency, we could store this mapping, but for now
  // we look it up by checking known built-in types and then registered types.

  // Try built-in types first.
  const google::protobuf::Descriptor* parent_desc = nullptr;
  for (const auto* desc :
       {TypeDefinition::descriptor(), TypeVersionDefinition::descriptor(), IndexDefinition::descriptor(), ReferenceDefinition::descriptor()}) {
    const auto& options = desc->options();
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      if (options.GetExtension(artifact_system::indexes, i).key_type() == key_type) {
        parent_desc = desc;
        break;
      }
    }
    if (parent_desc != nullptr)
      break;
  }

  // If not found in built-in types, we'd need to search registered types.
  // For now, only built-in types are supported for GetIndexSchema since
  // user types' descriptors need to be loaded from their TypeVersionDefinition.
  // TODO: Support user-type index schemas by loading the descriptor from the TVD.
  if (parent_desc == nullptr) {
    // Search all types with type_versions_by_type index.
    // For each TypeDefinition, load the current TypeVersionDefinition, get its
    // descriptor set, and check if it has this index.
    auto it = index_def_ids_by_key_type_.find("all_types");
    if (it != index_def_ids_by_key_type_.end()) {
      // Read all_types index (empty key).
      const auto* td_desc = TypeDefinition::descriptor();
      std::vector<uint8_t> empty_key;
      const auto& all_types_idx = td_desc->options().GetExtension(artifact_system::indexes, 1); // all_types is second
      auto all_ids_or = ReadIndexArtifactIds(ref, it->second, empty_key, all_types_idx, *td_desc);
      if (all_ids_or.ok()) {
        for (uint64_t td_id : *all_ids_or) {
          auto td_payload_or = ParseArtifactPayload<TypeDefinition>(storage_, ref, td_id);
          if (!td_payload_or.ok())
            continue;
          if (!td_payload_or->has_current_version_id())
            continue;
          auto tvd_payload_or = ParseArtifactPayload<TypeVersionDefinition>(storage_, ref, td_payload_or->current_version_id());
          if (!tvd_payload_or.ok())
            continue;
          google::protobuf::DescriptorPool user_pool(google::protobuf::DescriptorPool::generated_pool());
          const auto* user_desc = artifact::BuildPoolAndFindMessage(tvd_payload_or->descriptor_set(), td_payload_or->type_name(), &user_pool);
          if (user_desc == nullptr)
            continue;
          const auto& opts = user_desc->options();
          for (int i = 0; i < opts.ExtensionSize(artifact_system::indexes); ++i) {
            if (opts.GetExtension(artifact_system::indexes, i).key_type() == key_type) {
              auto schema_or = index::GenerateIndexSchema(idx_def, *user_desc);
              if (!schema_or.ok())
                return schema_or.status();
              return BuildIndexSchemaInfo(idx_id, idx_def, *schema_or);
            }
          }
        }
      }
    }
    return absl::NotFoundError(absl::StrCat("cannot find parent type for index '", key_type, "'"));
  }

  auto schema_or = index::GenerateIndexSchema(idx_def, *parent_desc);
  if (!schema_or.ok())
    return schema_or.status();
  return BuildIndexSchemaInfo(idx_id, idx_def, *schema_or);
}

} // namespace artifact_system::registry

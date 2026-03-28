#include "index/index_conflict_resolver.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/base64url.h"
#include "index/index_merge.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"

namespace artifact_system::index {
namespace {

using transaction::PathConflictKind;
using transaction::RetryResolutionContext;

struct ResolvedIndexPath {
  std::string path;
  std::string bytes;
};

absl::StatusOr<std::optional<std::string>> ReadObjectIfPresent(StorageInterface* storage, const std::string& ref, const std::string& path) {
  if (ref.empty()) {
    return std::optional<std::string>{};
  }
  auto object_or = storage->GetObject(ref, path);
  if (object_or.ok()) {
    return std::optional<std::string>(*object_or);
  }
  if (object_or.status().code() == absl::StatusCode::kNotFound) {
    return std::optional<std::string>{};
  }
  return object_or.status();
}

bool IndexDefinitionsEqual(const artifact_system::IndexDefinition& lhs, const artifact_system::IndexDefinition& rhs) {
  if (lhs.key_type() != rhs.key_type() || lhs.unique() != rhs.unique() || lhs.key_size() != rhs.key_size() || lhs.order_size() != rhs.order_size()) {
    return false;
  }
  for (int i = 0; i < lhs.key_size(); ++i) {
    if (lhs.key(i) != rhs.key(i)) {
      return false;
    }
  }
  for (int i = 0; i < lhs.order_size(); ++i) {
    if (lhs.order(i).field() != rhs.order(i).field() || lhs.order(i).direction() != rhs.order(i).direction()) {
      return false;
    }
  }
  return true;
}

std::vector<const google::protobuf::Descriptor*> CandidateIndexedDescriptors() {
  return {
      artifact_system::IndexDefinition::descriptor(),
      artifact_system::TypeDefinition::descriptor(),
      artifact_system::TypeVersionDefinition::descriptor(),
      artifact_system::ReferenceDefinition::descriptor(),
  };
}

absl::StatusOr<const google::protobuf::Descriptor*> ResolveParentDescriptorForIndexDefinition(const artifact_system::IndexDefinition& index_definition) {
  for (const google::protobuf::Descriptor* descriptor : CandidateIndexedDescriptors()) {
    const auto& options = descriptor->options();
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      if (IndexDefinitionsEqual(options.GetExtension(artifact_system::indexes, i), index_definition)) {
        return descriptor;
      }
    }
  }
  return absl::NotFoundError(absl::StrCat("no descriptor found for index key_type: ", index_definition.key_type()));
}

absl::StatusOr<uint64_t> ParseIndexDefinitionIdFromPath(const std::string& path) {
  if (path.rfind("indexes/", 0) != 0) {
    return absl::InvalidArgumentError(absl::StrCat("not an index path: ", path));
  }

  const size_t prefix_end = path.find('/', 8);
  if (prefix_end == std::string::npos) {
    return absl::InvalidArgumentError(absl::StrCat("invalid index path: ", path));
  }

  const std::string_view encoded_prefix = std::string_view(path).substr(8, prefix_end - 8);
  auto decoded_or = encoding::base64url::Decode(encoded_prefix);
  if (!decoded_or.ok() || decoded_or->size() != sizeof(uint64_t)) {
    return absl::InvalidArgumentError(absl::StrCat("invalid index path prefix encoding: ", path));
  }

  uint64_t id = 0;
  for (uint8_t byte : *decoded_or) {
    id = (id << 8U) | static_cast<uint64_t>(byte);
  }
  return id;
}

absl::StatusOr<artifact_system::IndexDefinition> LoadIndexDefinitionForConflict(StorageInterface* storage, const RetryResolutionContext& context,
                                                                                uint64_t index_definition_id) {
  const std::string definition_path = encoding::ArtifactPath(index_definition_id);
  const std::array<std::string, 3> refs = {
      context.merge_conflict.source_commit_id,
      context.merge_conflict.target_commit_id,
      context.merge_conflict.base_commit_id,
  };

  for (const std::string& ref : refs) {
    if (ref.empty()) {
      continue;
    }
    auto definition_bytes_or = storage->GetObject(ref, definition_path);
    if (!definition_bytes_or.ok()) {
      if (definition_bytes_or.status().code() == absl::StatusCode::kNotFound) {
        continue;
      }
      return definition_bytes_or.status();
    }
    artifact_system::IndexDefinition out;
    if (!out.ParseFromString(*definition_bytes_or)) {
      return absl::InvalidArgumentError(absl::StrCat("failed parsing index definition artifact payload for id: ", index_definition_id));
    }
    return out;
  }

  return absl::NotFoundError(absl::StrCat("index definition payload not found for id: ", index_definition_id));
}

absl::StatusOr<IndexObject> DeserializeIndexObjectWithSchema(const std::optional<std::string>& bytes, const GeneratedIndexSchema& schema,
                                                             const artifact_system::IndexDefinition& index_definition,
                                                             const std::string& fallback_serialized_key) {
  if (!bytes.has_value()) {
    return IndexObject{
        .serialized_key = fallback_serialized_key,
        .rows = {},
    };
  }

  auto object_or = DeserializeIndexObject(schema, index_definition, *bytes);
  if (!object_or.ok()) {
    return object_or.status();
  }
  return *object_or;
}

absl::StatusOr<std::optional<ResolvedIndexPath>> ResolveSinglePath(StorageInterface* storage, const RetryResolutionContext& context, const std::string& path) {
  auto index_definition_id_or = ParseIndexDefinitionIdFromPath(path);
  if (!index_definition_id_or.ok()) {
    return index_definition_id_or.status();
  }

  auto index_definition_or = LoadIndexDefinitionForConflict(storage, context, *index_definition_id_or);
  if (!index_definition_or.ok()) {
    return index_definition_or.status();
  }
  const artifact_system::IndexDefinition& index_definition = *index_definition_or;
  if (index_definition.unique()) {
    return std::optional<ResolvedIndexPath>{};
  }

  auto parent_descriptor_or = ResolveParentDescriptorForIndexDefinition(index_definition);
  if (!parent_descriptor_or.ok()) {
    if (parent_descriptor_or.status().code() == absl::StatusCode::kNotFound) {
      return std::optional<ResolvedIndexPath>{};
    }
    return parent_descriptor_or.status();
  }
  auto schema_or = GenerateIndexSchema(index_definition, **parent_descriptor_or);
  if (!schema_or.ok()) {
    return schema_or.status();
  }

  auto base_bytes_or = ReadObjectIfPresent(storage, context.merge_conflict.base_commit_id, path);
  if (!base_bytes_or.ok()) {
    return base_bytes_or.status();
  }
  auto ours_bytes_or = ReadObjectIfPresent(storage, context.merge_conflict.source_commit_id, path);
  if (!ours_bytes_or.ok()) {
    return ours_bytes_or.status();
  }
  auto theirs_bytes_or = ReadObjectIfPresent(storage, context.merge_conflict.target_commit_id, path);
  if (!theirs_bytes_or.ok()) {
    return theirs_bytes_or.status();
  }

  std::string serialized_key;
  if (base_bytes_or->has_value()) {
    auto parsed_or = DeserializeIndexObject(*schema_or, index_definition, **base_bytes_or);
    if (!parsed_or.ok()) {
      return parsed_or.status();
    }
    serialized_key = parsed_or->serialized_key;
  } else if (ours_bytes_or->has_value()) {
    auto parsed_or = DeserializeIndexObject(*schema_or, index_definition, **ours_bytes_or);
    if (!parsed_or.ok()) {
      return parsed_or.status();
    }
    serialized_key = parsed_or->serialized_key;
  } else if (theirs_bytes_or->has_value()) {
    auto parsed_or = DeserializeIndexObject(*schema_or, index_definition, **theirs_bytes_or);
    if (!parsed_or.ok()) {
      return parsed_or.status();
    }
    serialized_key = parsed_or->serialized_key;
  }

  auto base_object_or = DeserializeIndexObjectWithSchema(*base_bytes_or, *schema_or, index_definition, serialized_key);
  if (!base_object_or.ok()) {
    return base_object_or.status();
  }
  auto ours_object_or = DeserializeIndexObjectWithSchema(*ours_bytes_or, *schema_or, index_definition, serialized_key);
  if (!ours_object_or.ok()) {
    return ours_object_or.status();
  }
  auto theirs_object_or = DeserializeIndexObjectWithSchema(*theirs_bytes_or, *schema_or, index_definition, serialized_key);
  if (!theirs_object_or.ok()) {
    return theirs_object_or.status();
  }

  auto merged_or = MergeIndexObjects(index_definition, *base_object_or, *ours_object_or, *theirs_object_or);
  if (!merged_or.ok()) {
    return merged_or.status();
  }

  auto merged_bytes_or = SerializeIndexObject(*schema_or, index_definition, merged_or->merged);
  if (!merged_bytes_or.ok()) {
    return merged_bytes_or.status();
  }

  return std::optional<ResolvedIndexPath>(ResolvedIndexPath{
      .path = path,
      .bytes = *merged_bytes_or,
  });
}

} // namespace

PathConflictKind IndexPathConflictClassifier(const std::string& path) {
  if (path.rfind("indexes/", 0) == 0) {
    return PathConflictKind::kRetryableNonUniqueIndex;
  }
  return transaction::DefaultPathConflictClassifier(path);
}

transaction::PathConflictClassifier BuildIndexPathConflictClassifier(StorageInterface* storage) {
  return [storage](const std::string& path) -> PathConflictKind {
    if (path.rfind("indexes/", 0) != 0) {
      return transaction::DefaultPathConflictClassifier(path);
    }
    if (storage == nullptr) {
      return PathConflictKind::kNonRetryableUnknown;
    }

    auto index_definition_id_or = ParseIndexDefinitionIdFromPath(path);
    if (!index_definition_id_or.ok()) {
      return PathConflictKind::kNonRetryableUnknown;
    }

    auto definition_bytes_or = storage->GetObject(storage->GetCanonicalBranch(), encoding::ArtifactPath(*index_definition_id_or));
    if (!definition_bytes_or.ok()) {
      return PathConflictKind::kNonRetryableUnknown;
    }

    artifact_system::IndexDefinition definition;
    if (!definition.ParseFromString(*definition_bytes_or)) {
      return PathConflictKind::kNonRetryableUnknown;
    }

    return definition.unique() ? PathConflictKind::kNonRetryableUniqueIndex : PathConflictKind::kRetryableNonUniqueIndex;
  };
}

transaction::RetryConflictResolver BuildDeterministicIndexRetryConflictResolver(StorageInterface* storage) {
  return [storage](const RetryResolutionContext& context) -> absl::StatusOr<bool> {
    if (storage == nullptr) {
      return absl::FailedPreconditionError("storage is null");
    }
    if (context.merge_conflict.conflicting_paths.empty()) {
      return false;
    }

    bool wrote_anything = false;
    std::vector<ResolvedIndexPath> resolved_paths;
    resolved_paths.reserve(context.merge_conflict.conflicting_paths.size());
    for (const std::string& path : context.merge_conflict.conflicting_paths) {
      if (IndexPathConflictClassifier(path) != PathConflictKind::kRetryableNonUniqueIndex) {
        return false;
      }

      auto resolution_outcome_or = ResolveSinglePath(storage, context, path);
      if (!resolution_outcome_or.ok()) {
        return resolution_outcome_or.status();
      }
      if (!resolution_outcome_or->has_value()) {
        return false;
      }
      resolved_paths.push_back(std::move(**resolution_outcome_or));
      wrote_anything = true;
    }

    if (!wrote_anything) {
      return false;
    }

    for (const auto& resolved : resolved_paths) {
      absl::Status put_status = storage->PutObject(context.source_branch, resolved.path, resolved.bytes);
      if (!put_status.ok()) {
        return put_status;
      }
    }

    auto commit_or = storage->Commit(context.source_branch, absl::StrCat("deterministic index conflict resolution attempt ", context.attempts_performed));
    if (!commit_or.ok()) {
      return commit_or.status();
    }
    return true;
  };
}

} // namespace artifact_system::index

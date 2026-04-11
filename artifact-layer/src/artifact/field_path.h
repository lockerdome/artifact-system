#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "google/protobuf/descriptor.h"

namespace artifact_system::artifact {

// Resolve a potentially dotted field path (e.g. "foo.bar") against a protobuf
// descriptor. Returns the full chain of FieldDescriptors from root to leaf.
//
// Rejects:
//   - Empty paths or empty segments
//   - Fields that do not exist on the descriptor
//   - Repeated message intermediate segments
//   - Non-message intermediate segments (scalar in the middle of a path)
//   - Paths that resolve to a message (leaf must be scalar/enum)
//   - Map fields
inline absl::StatusOr<std::vector<const google::protobuf::FieldDescriptor*>> ResolveFieldPath(const google::protobuf::Descriptor& root,
                                                                                              std::string_view path) {
  if (path.empty()) {
    return absl::InvalidArgumentError("field path cannot be empty");
  }
  const google::protobuf::Descriptor* current = &root;
  std::vector<const google::protobuf::FieldDescriptor*> chain;
  const std::vector<std::string_view> segments = absl::StrSplit(path, '.');
  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string_view segment = segments[i];
    if (segment.empty()) {
      return absl::InvalidArgumentError(absl::StrCat("invalid field path: ", path));
    }
    const auto* field = current->FindFieldByName(segment);
    if (field == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat("field not found in path '", path, "': ", segment));
    }
    chain.push_back(field);
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        return absl::InvalidArgumentError(absl::StrCat("repeated message segments are not supported in field path: ", path));
      }
      current = field->message_type();
      continue;
    }
    if (i + 1 < segments.size()) {
      return absl::InvalidArgumentError(absl::StrCat("non-message intermediate segment in field path: ", path));
    }
  }
  if (chain.back()->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    return absl::InvalidArgumentError(absl::StrCat("field path must resolve to scalar/enum: ", path));
  }
  if (chain.back()->is_map()) {
    return absl::InvalidArgumentError(absl::StrCat("map fields are not supported for indexes: ", path));
  }
  return chain;
}

// Convenience wrapper: resolves a field path and returns just the leaf
// FieldDescriptor (the last element of the chain).
inline absl::StatusOr<const google::protobuf::FieldDescriptor*> ResolveFieldPathLeaf(const google::protobuf::Descriptor& root, std::string_view path) {
  auto chain_or = ResolveFieldPath(root, path);
  if (!chain_or.ok())
    return chain_or.status();
  return chain_or->back();
}

// ---------------------------------------------------------------------------
// Index-specific field path resolution.
//
// Unlike ResolveFieldPath, this variant:
//   - Allows repeated message intermediate segments (needed for nested repeated
//     field indexing, e.g., "inputs.types" where inputs is repeated message).
//   - Supports virtual "_index" segments that resolve to the 0-based position
//     of an element within its parent repeated field.

struct IndexFieldSegment {
  const google::protobuf::FieldDescriptor* descriptor; // nullptr for _index virtual segments.
  bool is_repeated;
  bool is_virtual_index; // true for _index segments.
};

struct ResolvedIndexFieldPath {
  std::vector<IndexFieldSegment> segments;
  bool leaf_is_virtual_index;
};

// Resolve a field path for index use. Returns the full segment chain with
// repeated-field metadata and virtual _index support.
inline absl::StatusOr<ResolvedIndexFieldPath> ResolveIndexFieldPath(const google::protobuf::Descriptor& root, std::string_view path) {
  if (path.empty()) {
    return absl::InvalidArgumentError("field path cannot be empty");
  }
  const google::protobuf::Descriptor* current = &root;
  ResolvedIndexFieldPath result;
  result.leaf_is_virtual_index = false;
  const std::vector<std::string_view> segments = absl::StrSplit(path, '.');
  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string_view segment = segments[i];
    if (segment.empty()) {
      return absl::InvalidArgumentError(absl::StrCat("invalid field path: ", path));
    }
    const bool is_last = (i + 1 == segments.size());

    // Virtual _index segment: must be the last segment and the preceding path
    // must have resolved to a repeated field.
    if (segment == "_index") {
      if (!is_last) {
        return absl::InvalidArgumentError(absl::StrCat("_index must be the last segment in field path: ", path));
      }
      if (result.segments.empty()) {
        return absl::InvalidArgumentError(absl::StrCat("_index requires a preceding repeated field in path: ", path));
      }
      if (!result.segments.back().is_repeated) {
        return absl::InvalidArgumentError(absl::StrCat("_index requires the preceding segment to be repeated in path: ", path));
      }
      IndexFieldSegment seg;
      seg.descriptor = nullptr;
      seg.is_repeated = false;
      seg.is_virtual_index = true;
      result.segments.push_back(seg);
      result.leaf_is_virtual_index = true;
      return result;
    }

    const auto* field = current->FindFieldByName(segment);
    if (field == nullptr) {
      return absl::InvalidArgumentError(absl::StrCat("field not found in path '", path, "': ", segment));
    }
    if (field->is_map()) {
      return absl::InvalidArgumentError(absl::StrCat("map fields are not supported for indexes: ", path));
    }

    IndexFieldSegment seg;
    seg.descriptor = field;
    seg.is_repeated = field->is_repeated();
    seg.is_virtual_index = false;
    result.segments.push_back(seg);

    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      // Allow repeated message intermediates (unlike ResolveFieldPath).
      current = field->message_type();
      if (is_last) {
        return absl::InvalidArgumentError(absl::StrCat("field path must resolve to scalar/enum: ", path));
      }
      continue;
    }
    // Scalar/enum: normally must be the leaf. Exception: a repeated scalar
    // followed by _index as the final segment (e.g., "inputs.types._index").
    if (!is_last) {
      const bool next_is_virtual_index = (i + 2 == segments.size() && segments[i + 1] == "_index");
      if (!next_is_virtual_index) {
        return absl::InvalidArgumentError(absl::StrCat("non-message intermediate segment in field path: ", path));
      }
      // Let the loop continue; the next iteration handles the _index segment.
      continue;
    }
  }
  return result;
}

// Convenience: extract the leaf FieldDescriptor from a ResolvedIndexFieldPath.
// Returns nullptr if the leaf is a virtual _index segment.
inline const google::protobuf::FieldDescriptor* IndexFieldPathLeaf(const ResolvedIndexFieldPath& resolved) {
  if (resolved.segments.empty()) {
    return nullptr;
  }
  return resolved.segments.back().descriptor;
}

} // namespace artifact_system::artifact

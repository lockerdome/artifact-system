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

} // namespace artifact_system::artifact

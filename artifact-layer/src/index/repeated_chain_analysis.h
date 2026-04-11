#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "artifact/field_path.h"
#include "google/protobuf/descriptor.h"

namespace artifact_system::index {

// Identifies a repeated field at a specific position in the ancestry chain.
struct RepeatedAncestor {
  std::string path_prefix; // e.g., "inputs" or "inputs.types"
  const google::protobuf::FieldDescriptor* descriptor;
};

// Result of analyzing all field paths in an index definition for their repeated
// ancestry structure. The chain is the single linear sequence of repeated fields
// from root to deepest; field_depth maps each field path to its depth in the chain.
struct RepeatedChainAnalysis {
  // Linear chain of repeated ancestors, ordered root-to-deepest.
  // E.g., for keys ["inputs.types", "inputs.name"]:
  //   chain = [{"inputs", inputs_fd}, {"inputs.types", types_fd}]
  std::vector<RepeatedAncestor> chain;

  // For each field path, its depth in the chain.
  // Depth 0 = root-level (no repeated ancestry).
  // Depth N = lives inside the Nth repeated ancestor.
  // _index virtual fields get the same depth as their parent repeated.
  std::unordered_map<std::string, int> field_depth;
};

namespace internal {

// Extract the repeated ancestor chain from a single resolved index field path.
// Returns the sequence of (path_prefix, descriptor) pairs for each repeated segment.
inline std::vector<RepeatedAncestor> ExtractRepeatedAncestors(const artifact::ResolvedIndexFieldPath& resolved, std::string_view path) {
  std::vector<RepeatedAncestor> ancestors;
  std::string prefix;
  const std::vector<std::string_view> segments = absl::StrSplit(path, '.');
  for (size_t i = 0; i < resolved.segments.size(); ++i) {
    const auto& seg = resolved.segments[i];
    if (seg.is_virtual_index) {
      break; // _index is not itself a repeated ancestor.
    }
    if (!prefix.empty()) {
      prefix.push_back('.');
    }
    prefix.append(segments[i].data(), segments[i].size());
    if (seg.is_repeated) {
      ancestors.push_back({prefix, seg.descriptor});
    }
  }
  return ancestors;
}

// Compute the depth of a field path given its resolved segments and the global chain.
// Returns -1 on error (field introduces a repeated not in the chain).
inline int ComputeFieldDepth(const artifact::ResolvedIndexFieldPath& resolved, std::string_view path,
                             const std::vector<RepeatedAncestor>& chain) {
  auto ancestors = ExtractRepeatedAncestors(resolved, path);

  if (resolved.leaf_is_virtual_index) {
    // _index is at the same depth as its parent repeated.
    // The parent repeated is the last ancestor in this field's chain.
    if (ancestors.empty()) {
      return -1; // _index with no repeated parent -- should not happen (caught by ResolveIndexFieldPath).
    }
    // Find the depth of the parent repeated in the global chain.
    const std::string& parent_path = ancestors.back().path_prefix;
    for (size_t i = 0; i < chain.size(); ++i) {
      if (chain[i].path_prefix == parent_path) {
        return static_cast<int>(i + 1);
      }
    }
    return -1; // Parent repeated not in chain.
  }

  // For non-virtual fields, depth = number of repeated ancestors in the chain
  // that this field traverses through.
  int depth = 0;
  for (const auto& ancestor : ancestors) {
    bool found = false;
    for (size_t i = 0; i < chain.size(); ++i) {
      if (chain[i].path_prefix == ancestor.path_prefix) {
        depth = static_cast<int>(i + 1);
        found = true;
        break;
      }
    }
    if (!found) {
      return -1; // This field traverses a repeated not in the chain.
    }
  }
  return depth;
}

} // namespace internal

// Analyze all field paths in an index definition and validate that repeated
// fields form a single linear ancestry chain. Returns the chain structure
// and per-field depth assignments.
//
// key_paths: field paths from the index key fields.
// order_paths: field paths from the index order fields (excluding "artifact_id",
//              which should be filtered by the caller).
inline absl::StatusOr<RepeatedChainAnalysis> AnalyzeRepeatedChain(const google::protobuf::Descriptor& root,
                                                                  const std::vector<std::string>& key_paths,
                                                                  const std::vector<std::string>& order_paths) {
  RepeatedChainAnalysis analysis;

  // Phase 1: Build the global repeated chain from key paths.
  // Each key path contributes its repeated ancestors; they must all be
  // prefixes of or equal to one single linear chain.
  std::vector<RepeatedAncestor> global_chain;

  for (const auto& key_path : key_paths) {
    auto resolved_or = artifact::ResolveIndexFieldPath(root, key_path);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    auto ancestors = internal::ExtractRepeatedAncestors(*resolved_or, key_path);

    // Merge this path's ancestors into the global chain.
    // The ancestors must be a prefix of or extend the global chain.
    for (size_t i = 0; i < ancestors.size(); ++i) {
      if (i < global_chain.size()) {
        // Must match existing chain at this position.
        if (global_chain[i].path_prefix != ancestors[i].path_prefix) {
          return absl::InvalidArgumentError(absl::StrCat(
              "INVALID_INDEX_DEFINITION: branching repeated paths in index keys: '",
              global_chain[i].path_prefix, "' vs '", ancestors[i].path_prefix,
              "' at depth ", i));
        }
      } else {
        // Extends the chain.
        global_chain.push_back(ancestors[i]);
      }
    }
  }
  analysis.chain = global_chain;

  // Phase 2: Assign depths for key paths.
  for (const auto& key_path : key_paths) {
    auto resolved_or = artifact::ResolveIndexFieldPath(root, key_path);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    int depth = internal::ComputeFieldDepth(*resolved_or, key_path, analysis.chain);
    if (depth < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          "INVALID_INDEX_DEFINITION: key field '", key_path,
          "' references a repeated field not in the ancestry chain"));
    }
    analysis.field_depth[key_path] = depth;
  }

  // Phase 3: Validate order paths and assign depths.
  for (const auto& order_path : order_paths) {
    auto resolved_or = artifact::ResolveIndexFieldPath(root, order_path);
    if (!resolved_or.ok()) {
      return resolved_or.status();
    }
    auto ancestors = internal::ExtractRepeatedAncestors(*resolved_or, order_path);

    // Check that every repeated ancestor in this order path exists in the global chain.
    for (const auto& ancestor : ancestors) {
      bool found = false;
      for (const auto& chain_entry : global_chain) {
        if (chain_entry.path_prefix == ancestor.path_prefix) {
          found = true;
          break;
        }
      }
      if (!found) {
        return absl::InvalidArgumentError(absl::StrCat(
            "INVALID_INDEX_DEFINITION: order field '", order_path,
            "' introduces repeated path '", ancestor.path_prefix,
            "' not expanded by key fields"));
      }
    }

    int depth = internal::ComputeFieldDepth(*resolved_or, order_path, analysis.chain);
    if (depth < 0) {
      return absl::InvalidArgumentError(absl::StrCat(
          "INVALID_INDEX_DEFINITION: order field '", order_path,
          "' references a repeated field not in the ancestry chain"));
    }
    analysis.field_depth[order_path] = depth;
  }

  return analysis;
}

} // namespace artifact_system::index

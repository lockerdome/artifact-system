#pragma once

#include "absl/status/statusor.h"
#include "artifact_options.pb.h"
#include "index/index_object.h"

namespace artifact_system::index {

struct IndexMergeResult {
  IndexObject merged;
  bool unique_conflict = false;
};

absl::StatusOr<IndexMergeResult> MergeIndexObjects(const artifact_system::IndexDefinition& index_definition, const IndexObject& base, const IndexObject& ours,
                                                   const IndexObject& theirs);

} // namespace artifact_system::index

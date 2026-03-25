#include "index/index_merge.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace artifact_system::index {
namespace {

using RowMap = std::map<uint64_t, IndexRow>;

absl::StatusOr<int> CompareRowsByOrder(const artifact_system::IndexDefinition& index_definition, const IndexRow& lhs, const IndexRow& rhs) {
  const size_t expected = static_cast<size_t>(index_definition.order_size());
  if (lhs.order_values.size() != expected || rhs.order_values.size() != expected) {
    return absl::InvalidArgumentError("cannot compare rows with different order value lengths");
  }

  for (int i = 0; i < index_definition.order_size(); ++i) {
    auto cmp_or = CompareIndexCellAscending(lhs.order_values[i], rhs.order_values[i]);
    if (!cmp_or.ok()) {
      return cmp_or.status();
    }
    const int cmp = *cmp_or;
    if (cmp == 0) {
      continue;
    }
    if (index_definition.order(i).direction() == artifact_system::OrderDefinition::DESCENDING) {
      return -cmp;
    }
    return cmp;
  }
  return 0;
}

bool RowsEqual(const IndexRow& lhs, const IndexRow& rhs) {
  return lhs.artifact_id == rhs.artifact_id && lhs.order_values == rhs.order_values;
}

absl::StatusOr<int> CompareRowsCanonical(const IndexRow& lhs, const IndexRow& rhs) {
  if (lhs.order_values.size() != rhs.order_values.size()) {
    return absl::InvalidArgumentError("cannot compare rows with different order value lengths");
  }
  for (size_t i = 0; i < lhs.order_values.size(); ++i) {
    auto cmp_or = CompareIndexCellAscending(lhs.order_values[i], rhs.order_values[i]);
    if (!cmp_or.ok()) {
      return cmp_or.status();
    }
    if (*cmp_or != 0) {
      return *cmp_or;
    }
  }
  return 0;
}

struct Delta {
  std::set<uint64_t> removes;
  std::vector<IndexRow> adds;
};

Delta ComputeDelta(const RowMap& base, const RowMap& head) {
  Delta delta;

  for (const auto& [artifact_id, base_row] : base) {
    const auto head_it = head.find(artifact_id);
    if (head_it == head.end()) {
      delta.removes.insert(artifact_id);
      continue;
    }
    if (!RowsEqual(base_row, head_it->second)) {
      delta.removes.insert(artifact_id);
      delta.adds.push_back(head_it->second);
    }
  }

  for (const auto& [artifact_id, head_row] : head) {
    if (!base.contains(artifact_id)) {
      delta.adds.push_back(head_row);
    }
  }

  return delta;
}

absl::StatusOr<RowMap> ToRowMap(const IndexObject& object) {
  RowMap out;
  for (const IndexRow& row : object.rows) {
    if (out.contains(row.artifact_id)) {
      return absl::InvalidArgumentError("duplicate artifact_id rows are not allowed");
    }
    out[row.artifact_id] = row;
  }
  return out;
}

} // namespace

absl::StatusOr<IndexMergeResult> MergeIndexObjects(const artifact_system::IndexDefinition& index_definition, const IndexObject& base, const IndexObject& ours,
                                                   const IndexObject& theirs) {
  if (base.serialized_key != ours.serialized_key || base.serialized_key != theirs.serialized_key) {
    return absl::InvalidArgumentError("index merge requires base/ours/theirs to have same key");
  }

  bool has_artifact_id_order = false;
  for (const auto& order : index_definition.order()) {
    if (order.field() == "artifact_id") {
      has_artifact_id_order = true;
    }
    if (order.direction() == artifact_system::OrderDefinition::ORDER_BY_UNSPECIFIED) {
      return absl::InvalidArgumentError("index merge requires explicit order direction");
    }
  }
  if (!has_artifact_id_order) {
    return absl::InvalidArgumentError("index merge requires artifact_id order field");
  }

  auto base_map_or = ToRowMap(base);
  if (!base_map_or.ok()) {
    return base_map_or.status();
  }
  auto ours_map_or = ToRowMap(ours);
  if (!ours_map_or.ok()) {
    return ours_map_or.status();
  }
  auto theirs_map_or = ToRowMap(theirs);
  if (!theirs_map_or.ok()) {
    return theirs_map_or.status();
  }

  const RowMap& base_map = *base_map_or;
  const RowMap& ours_map = *ours_map_or;
  const RowMap& theirs_map = *theirs_map_or;

  const Delta ours_delta = ComputeDelta(base_map, ours_map);
  const Delta theirs_delta = ComputeDelta(base_map, theirs_map);

  RowMap merged = base_map;
  for (uint64_t artifact_id : ours_delta.removes) {
    merged.erase(artifact_id);
  }
  for (uint64_t artifact_id : theirs_delta.removes) {
    merged.erase(artifact_id);
  }

  std::map<uint64_t, std::vector<IndexRow>> candidate_adds;
  for (const IndexRow& row : ours_delta.adds) {
    candidate_adds[row.artifact_id].push_back(row);
  }
  for (const IndexRow& row : theirs_delta.adds) {
    candidate_adds[row.artifact_id].push_back(row);
  }

  for (auto& [artifact_id, candidates] : candidate_adds) {
    IndexRow chosen = candidates.front();
    for (size_t i = 1; i < candidates.size(); ++i) {
      auto cmp_or = CompareRowsCanonical(candidates[i], chosen);
      if (!cmp_or.ok()) {
        return cmp_or.status();
      }
      if (*cmp_or < 0) {
        chosen = candidates[i];
      }
    }
    merged[artifact_id] = std::move(chosen);
  }

  IndexObject merged_object;
  merged_object.serialized_key = base.serialized_key;
  merged_object.rows.reserve(merged.size());
  const size_t expected_order_count = static_cast<size_t>(index_definition.order_size());
  for (const auto& [artifact_id, row] : merged) {
    if (row.order_values.size() != expected_order_count) {
      return absl::InvalidArgumentError("merged row order value count does not match index definition");
    }
    for (const auto& cell : row.order_values) {
      auto self_cmp_or = CompareIndexCellAscending(cell, cell);
      if (!self_cmp_or.ok()) {
        return self_cmp_or.status();
      }
    }
    merged_object.rows.push_back(row);
    merged_object.rows.back().artifact_id = artifact_id;
  }

  for (size_t i = 0; i < merged_object.rows.size(); ++i) {
    for (size_t j = i + 1; j < merged_object.rows.size(); ++j) {
      auto cmp_or = CompareRowsByOrder(index_definition, merged_object.rows[i], merged_object.rows[j]);
      if (!cmp_or.ok()) {
        return cmp_or.status();
      }
    }
  }

  std::sort(merged_object.rows.begin(), merged_object.rows.end(), [&](const IndexRow& lhs, const IndexRow& rhs) {
    const auto cmp_or = CompareRowsByOrder(index_definition, lhs, rhs);
    if (*cmp_or != 0) {
      return *cmp_or < 0;
    }
    return lhs.artifact_id < rhs.artifact_id;
  });

  IndexMergeResult result;
  result.merged = std::move(merged_object);
  if (index_definition.unique() && result.merged.rows.size() > 1) {
    result.unique_conflict = true;
  }
  return result;
}

} // namespace artifact_system::index

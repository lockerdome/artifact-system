#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "artifact_options.pb.h"
#include "index/index_schema_generator.h"

namespace artifact_system::index {

struct BytesValue {
  std::string value;

  bool operator==(const BytesValue& other) const {
    return value == other.value;
  }
};

using IndexCell = std::variant<int32_t, uint32_t, int64_t, uint64_t, bool, float, double, std::string, BytesValue>;

struct IndexRow {
  uint64_t artifact_id = 0;
  std::vector<IndexCell> order_values;
};

struct IndexObject {
  std::string serialized_key;
  std::vector<IndexRow> rows;
};

absl::StatusOr<std::string> SerializeIndexObject(const GeneratedIndexSchema& schema, const artifact_system::IndexDefinition& index_definition,
                                                 const IndexObject& object);

absl::StatusOr<IndexObject> DeserializeIndexObject(const GeneratedIndexSchema& schema, const artifact_system::IndexDefinition& index_definition,
                                                   const std::string& bytes);

absl::StatusOr<int> CompareIndexCellAscending(const IndexCell& lhs, const IndexCell& rhs);

} // namespace artifact_system::index

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.h"

#include "artifact_options.pb.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "storage/storage_interface.h"

namespace artifact_system::index {

// Find an IndexDefinition by key_type from descriptor message options.
std::optional<IndexDefinition> FindIndexDefinition(const google::protobuf::Descriptor& descriptor, const std::string& key_type);

// Build a proto-serialized key message from raw key values and a generated
// index schema. Returns an empty string for empty key_values.
absl::StatusOr<std::string> BuildProtoSerializedKey(const GeneratedIndexSchema& schema, const std::vector<IndexCell>& key_values);

// Add an index row for a derived entry. Reads the existing index object
// (if any), appends the row, and writes back.
absl::Status AddIndexRow(StorageInterface* storage, const std::string& branch, const DerivedIndexEntry& entry, uint64_t artifact_id,
                         const IndexDefinition& index_def, const google::protobuf::Descriptor& descriptor);

// Remove an index row for a derived entry. Reads the existing index object,
// erases the row matching artifact_id, and writes back. Best-effort: returns
// OkStatus on any intermediate failure.
absl::Status RemoveIndexRow(StorageInterface* storage, const std::string& branch, const DerivedIndexEntry& entry, uint64_t artifact_id,
                            const IndexDefinition& index_def, const google::protobuf::Descriptor& descriptor);

} // namespace artifact_system::index

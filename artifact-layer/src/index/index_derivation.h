#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/message.h"
#include "index/index_object.h"

namespace artifact_system::index {

struct DerivedIndexEntry {
  uint64_t index_def_id = 0;
  std::string key_type;
  std::vector<uint8_t> encoded_key;    // Custom binary encoding for storage paths.
  std::vector<IndexCell> order_values;
  std::vector<IndexCell> key_values;   // Raw key field values (for building proto-serialized keys).
};

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntries(const google::protobuf::Descriptor& descriptor,
                                                                  const google::protobuf::Message& artifact_message, uint64_t artifact_id,
                                                                  const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type);

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntries(const google::protobuf::Descriptor& descriptor,
                                                                  const google::protobuf::Message& artifact_message, uint64_t artifact_id);

absl::StatusOr<std::vector<DerivedIndexEntry>> DeriveIndexEntriesFromPayload(const google::protobuf::FileDescriptorSet& descriptor_set,
                                                                             const std::string& message_full_name, const std::string& payload,
                                                                             uint64_t artifact_id,
                                                                             const std::unordered_map<std::string, uint64_t>& index_def_ids_by_key_type);

} // namespace artifact_system::index

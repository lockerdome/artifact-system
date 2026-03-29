#pragma once

#include <string>
#include <vector>

#include "google/protobuf/descriptor.pb.h"

namespace artifact_system::registry {

struct SchemaViolation {
  std::string description; // Human-readable, e.g. "field 'created_by' changed type from uint64 to string"
  std::string subject;     // e.g. "field: created_by" or "field_number: 3"
};

// Compare old_descriptor_set against new_descriptor_set for backward compatibility.
// type_name identifies the root message to compare (fully-qualified, e.g. "mypackage.MyMessage").
// Returns empty vector if compatible, or a list of violations.
std::vector<SchemaViolation> CheckSchemaCompatibility(const google::protobuf::FileDescriptorSet& old_descriptor_set,
                                                      const google::protobuf::FileDescriptorSet& new_descriptor_set, const std::string& type_name);

} // namespace artifact_system::registry

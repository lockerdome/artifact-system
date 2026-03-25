#pragma once

#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "artifact_options.pb.h"
#include "google/protobuf/descriptor.h"

namespace artifact_system::index {

struct GeneratedIndexSchema {
  std::shared_ptr<google::protobuf::DescriptorPool> pool;
  const google::protobuf::FileDescriptor* file_descriptor = nullptr;
  const google::protobuf::Descriptor* key_descriptor = nullptr;
  const google::protobuf::Descriptor* value_descriptor = nullptr;
  const google::protobuf::Descriptor* index_descriptor = nullptr;
  std::vector<const google::protobuf::FieldDescriptor*> value_fields;
};

absl::StatusOr<GeneratedIndexSchema> GenerateIndexSchema(const artifact_system::IndexDefinition& index_definition,
                                                         const google::protobuf::Descriptor& parent_descriptor);

} // namespace artifact_system::index

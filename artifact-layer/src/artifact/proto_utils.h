#pragma once

#include <string>
#include <vector>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact_service.pb.h"

namespace artifact_system::artifact {

// Build a DescriptorPool from a FileDescriptorSet and find a message by name.
// Uses a topological-sort loop to handle dependency ordering.
// Returns nullptr if the message is not found.
inline const google::protobuf::Descriptor* BuildPoolAndFindMessage(const google::protobuf::FileDescriptorSet& descriptor_set,
                                                                   const std::string& message_full_name, google::protobuf::DescriptorPool* pool) {
  std::vector<bool> built(descriptor_set.file_size(), false);
  int built_count = 0;
  bool made_progress = true;
  while (built_count < descriptor_set.file_size() && made_progress) {
    made_progress = false;
    for (int i = 0; i < descriptor_set.file_size(); ++i) {
      if (built[static_cast<size_t>(i)]) {
        continue;
      }
      const auto& file = descriptor_set.file(i);
      if (pool->FindFileByName(file.name()) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
        continue;
      }
      if (pool->BuildFile(file) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
      }
    }
  }
  return pool->FindMessageTypeByName(message_full_name);
}

// Helper: build a single ArtifactWriteViolation.
inline ArtifactWriteViolation MakeViolation(ArtifactWriteViolation::Category category, const std::string& subject, const std::string& description) {
  ArtifactWriteViolation v;
  v.set_category(category);
  v.set_subject(subject);
  v.set_description(description);
  return v;
}

} // namespace artifact_system::artifact

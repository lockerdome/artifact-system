#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "encoding/artifact_path.h"
#include "storage/storage_interface.h"

namespace artifact_system::artifact {

// Build a FileDescriptorSet from a compiled-in Descriptor, recursively
// including all transitive file dependencies.
inline google::protobuf::FileDescriptorSet BuildDescriptorSet(const google::protobuf::Descriptor* desc) {
  google::protobuf::FileDescriptorSet fds;
  std::set<const google::protobuf::FileDescriptor*> seen;
  std::function<void(const google::protobuf::FileDescriptor*)> add_file;
  add_file = [&](const google::protobuf::FileDescriptor* fd) {
    if (!seen.insert(fd).second)
      return;
    for (int i = 0; i < fd->dependency_count(); ++i) {
      add_file(fd->dependency(i));
    }
    fd->CopyTo(fds.add_file());
  };
  add_file(desc->file());
  return fds;
}

// Serialize a StoredArtifact envelope with the given fields.
inline std::string SerializeStoredArtifact(uint64_t version_id, std::string_view type_name, const std::string& payload) {
  StoredArtifact envelope;
  envelope.set_envelope_version(1);
  envelope.set_version_id(version_id);
  envelope.set_type_name(type_name);
  envelope.set_payload(payload);
  return envelope.SerializeAsString();
}

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

// Read and parse a StoredArtifact from storage. Returns NOT_FOUND if the
// object does not exist.
inline absl::StatusOr<StoredArtifact> ReadStoredArtifact(StorageInterface* storage, const std::string& ref, uint64_t artifact_id) {
  const std::string path = encoding::ArtifactPath(artifact_id);
  auto data_or = storage->GetObject(ref, path);
  if (!data_or.ok()) {
    return data_or.status();
  }
  StoredArtifact stored;
  if (!stored.ParseFromString(*data_or)) {
    return absl::InternalError(absl::StrCat("failed to parse StoredArtifact at ", path));
  }
  return stored;
}

// Like ReadStoredArtifact, but returns nullopt instead of NOT_FOUND.
inline absl::StatusOr<std::optional<StoredArtifact>> ReadStoredArtifactIfExists(StorageInterface* storage, const std::string& ref, uint64_t artifact_id) {
  auto result = ReadStoredArtifact(storage, ref, artifact_id);
  if (!result.ok()) {
    if (absl::IsNotFound(result.status())) {
      return std::nullopt;
    }
    return result.status();
  }
  return std::move(*result);
}

} // namespace artifact_system::artifact

#include "service/env_config.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "id/id_allocator.h"
#include "storage/lakefs_config.h"

namespace artifact_system::service {
namespace {

std::string GetEnvOrDefault(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  if (value != nullptr && value[0] != '\0') {
    return value;
  }
  return fallback;
}

std::string GetEnvOrEmpty(const char* name) {
  return GetEnvOrDefault(name, "");
}

absl::StatusOr<LakeFSConfig> LoadLakeFSConfig() {
  LakeFSConfig lakefs;
  lakefs.endpoint = GetEnvOrEmpty("LAKEFS_ENDPOINT");
  lakefs.access_key_id = GetEnvOrEmpty("LAKEFS_ACCESS_KEY_ID");
  lakefs.secret_access_key = GetEnvOrEmpty("LAKEFS_SECRET_ACCESS_KEY");
  lakefs.repository = GetEnvOrEmpty("LAKEFS_REPOSITORY");
  lakefs.canonical_branch = GetEnvOrDefault("LAKEFS_CANONICAL_BRANCH", "main");

  std::vector<std::string> missing;
  if (lakefs.endpoint.empty()) {
    missing.push_back("LAKEFS_ENDPOINT");
  }
  if (lakefs.access_key_id.empty()) {
    missing.push_back("LAKEFS_ACCESS_KEY_ID");
  }
  if (lakefs.secret_access_key.empty()) {
    missing.push_back("LAKEFS_SECRET_ACCESS_KEY");
  }
  if (lakefs.repository.empty()) {
    missing.push_back("LAKEFS_REPOSITORY");
  }
  if (!missing.empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("ARTIFACT_LAYER_STORAGE_TYPE=lakefs requires: ", absl::StrJoin(missing, ", ")));
  }
  return lakefs;
}

absl::StatusOr<IdAllocatorConfig> LoadIdAllocatorConfig(std::string service_address) {
  IdAllocatorConfig allocator;
  allocator.service_address = std::move(service_address);
  allocator.partition_id = GetEnvOrEmpty("ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID");
  if (allocator.partition_id.empty()) {
    return absl::InvalidArgumentError(
        "ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS requires ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID");
  }

  const std::string high_water_mark = GetEnvOrEmpty("ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK");
  if (!high_water_mark.empty()) {
    uint64_t parsed = 0;
    if (!absl::SimpleAtoi(high_water_mark, &parsed) || parsed == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK must be a positive integer, got \"",
                       high_water_mark, "\""));
    }
    allocator.high_water_mark = parsed;
  }
  return allocator;
}

} // namespace

absl::StatusOr<ServerConfig> LoadServerConfigFromEnv() {
  ServerConfig config;

  const std::string listen_address = GetEnvOrEmpty("ARTIFACT_LAYER_LISTEN_ADDRESS");
  if (!listen_address.empty()) {
    config.listen_address = listen_address;
  }

  const std::string storage_type = GetEnvOrDefault("ARTIFACT_LAYER_STORAGE_TYPE", "memory");
  if (storage_type == "lakefs") {
    auto lakefs = LoadLakeFSConfig();
    if (!lakefs.ok()) {
      return lakefs.status();
    }
    config.lakefs = std::move(lakefs).value();
  } else if (storage_type != "memory") {
    return absl::InvalidArgumentError(absl::StrCat("Unknown ARTIFACT_LAYER_STORAGE_TYPE \"", storage_type,
                                                   "\" (expected \"memory\" or \"lakefs\")"));
  }

  const std::string allocator_address = GetEnvOrEmpty("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS");
  if (!allocator_address.empty()) {
    auto allocator = LoadIdAllocatorConfig(allocator_address);
    if (!allocator.ok()) {
      return allocator.status();
    }
    config.id_allocator = std::move(allocator).value();
  }

  // LakeFS storage survives restarts but MockIdAllocator restarts from the
  // first user-allocatable ID, so it would re-issue IDs already present in
  // storage.
  if (config.lakefs.has_value() && !config.id_allocator.has_value()) {
    return absl::InvalidArgumentError(
        "ARTIFACT_LAYER_STORAGE_TYPE=lakefs requires ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS: persistent "
        "storage cannot run with the mock ID allocator");
  }

  return config;
}

} // namespace artifact_system::service

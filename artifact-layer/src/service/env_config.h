#pragma once

#include "absl/status/statusor.h"
#include "service/server.h"

namespace artifact_system::service {

// Build a ServerConfig from environment variables.
//
//   ARTIFACT_LAYER_LISTEN_ADDRESS   listen address (default "0.0.0.0:50051")
//   ARTIFACT_LAYER_STORAGE_TYPE     "memory" (default) or "lakefs"
//
// When ARTIFACT_LAYER_STORAGE_TYPE=lakefs:
//   LAKEFS_ENDPOINT                 LakeFS API endpoint URL (required)
//   LAKEFS_ACCESS_KEY_ID            access key ID (required)
//   LAKEFS_SECRET_ACCESS_KEY        secret access key (required; injected at
//                                   deploy time from Secret Manager)
//   LAKEFS_REPOSITORY               repository name (required)
//   LAKEFS_CANONICAL_BRANCH         canonical branch (default "main")
//
// ID allocator (required when storage type is "lakefs" — persistent storage
// with the mock allocator would hand out already-used IDs after a restart):
//   ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS          id-allocator service address
//   ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID     partition ID (required with
//                                                the address)
//   ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK  refill threshold (optional)
//
// Returns InvalidArgumentError describing the offending variables when the
// combination is unusable.
absl::StatusOr<ServerConfig> LoadServerConfigFromEnv();

} // namespace artifact_system::service

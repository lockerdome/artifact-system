#pragma once

#include <string>

namespace artifact_system {

/// Configuration for connecting to a LakeFS instance.
///
/// All fields are required for production use. For testing, endpoint and
/// credentials can point to a local Docker LakeFS instance.
struct LakeFSConfig {
  /// LakeFS API endpoint URL (e.g., "http://localhost:8000").
  /// Should not include a trailing slash or "/api/v1" — those are appended
  /// internally.
  std::string endpoint;

  /// LakeFS access key ID for authentication.
  std::string access_key_id;

  /// LakeFS secret access key for authentication.
  std::string secret_access_key;

  /// Name of the LakeFS repository to use for all operations.
  std::string repository;

  /// Name of the canonical (main) branch. Defaults to "main".
  std::string canonical_branch = "main";

  /// HTTP request timeout in seconds. Zero means no timeout.
  long timeout_seconds = 30;
};

} // namespace artifact_system

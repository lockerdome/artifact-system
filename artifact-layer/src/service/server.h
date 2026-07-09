#pragma once
#include "absl/status/status.h"
#include "grpcpp/server.h"
#include "id/id_allocator.h"
#include "storage/lakefs_config.h"
#include <memory>
#include <optional>
#include <string>

namespace artifact_system::service {

struct ServerConfig {
  std::string listen_address = "0.0.0.0:50051";

  // When set, use LakeFS-backed storage connecting to this instance.
  // When empty, use MemoryStorage (for tests; all data is lost on shutdown).
  std::optional<LakeFSConfig> lakefs;

  // When set, use the production ID allocator connecting to this service.
  // When empty, use MockIdAllocator (for tests).
  std::optional<IdAllocatorConfig> id_allocator;
};

// Owns the gRPC server and all service dependencies.
class ArtifactLayerServer {
public:
  explicit ArtifactLayerServer(const ServerConfig& config = {});
  ~ArtifactLayerServer();

  // Non-copyable, non-movable (owns gRPC server)
  ArtifactLayerServer(const ArtifactLayerServer&) = delete;
  ArtifactLayerServer& operator=(const ArtifactLayerServer&) = delete;

  // Initialize all components (storage, genesis, services).
  // Must be called before Start().
  absl::Status Initialize();

  // Start the gRPC server. Blocks until Shutdown() is called.
  void Start();

  // Shutdown the server gracefully.
  void Shutdown();

  // Returns the server's listening port (useful for tests).
  int port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace artifact_system::service

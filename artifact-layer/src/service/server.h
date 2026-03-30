#pragma once
#include <memory>
#include <string>
#include "absl/status/status.h"
#include "grpcpp/server.h"

namespace artifact_system::service {

struct ServerConfig {
  std::string listen_address = "0.0.0.0:50051";
};

// Owns the gRPC server and all service dependencies.
// Uses MemoryStorage for now (P10 adds LakeFS option).
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

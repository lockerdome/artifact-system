#include "absl/status/status.h"
#include "service/server.h"
#include <csignal>
#include <print>

namespace {
artifact_system::service::ArtifactLayerServer* g_server = nullptr;

void SignalHandler(int) {
  if (g_server)
    g_server->Shutdown();
}
} // namespace

int main() {
  artifact_system::service::ServerConfig config;
  // TODO: parse config from flags/env

  artifact_system::service::ArtifactLayerServer server(config);
  g_server = &server;

  auto status = server.Initialize();
  if (!status.ok()) {
    std::println(stderr, "Failed to initialize server: {}", std::string(status.message()));
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::println("Starting artifact layer server...");
  server.Start();

  return 0;
}

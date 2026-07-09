#include "absl/status/status.h"
#include "service/env_config.h"
#include "service/server.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <print>
#include <thread>

namespace {
std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int) {
  g_shutdown_requested.store(true, std::memory_order_relaxed);
}
} // namespace

int main() {
  auto config = artifact_system::service::LoadServerConfigFromEnv();
  if (!config.ok()) {
    std::println(stderr, "Invalid server configuration: {}", std::string(config.status().message()));
    return 1;
  }

  if (config->lakefs.has_value()) {
    std::println("Using LakeFS storage (endpoint={}, repository={}, branch={})", config->lakefs->endpoint,
                 config->lakefs->repository, config->lakefs->canonical_branch);
  } else {
    std::println("Using in-memory storage");
  }
  if (config->id_allocator.has_value()) {
    std::println("Using production ID allocator (address={}, partition={})", config->id_allocator->service_address,
                 config->id_allocator->partition_id);
  } else {
    std::println("Using mock ID allocator");
  }

  artifact_system::service::ArtifactLayerServer server(config.value());

  auto status = server.Initialize();
  if (!status.ok()) {
    std::println(stderr, "Failed to initialize server: {}", std::string(status.message()));
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::thread server_thread([&server] {
    server.Start();
    // If Start() returns without a shutdown request, startup failed
    // (e.g. BuildAndStart() returned nullptr due to a bad listen address).
    // Set the shutdown flag so main() stops waiting.
    g_shutdown_requested.store(true, std::memory_order_relaxed);
  });

  while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  server.Shutdown();
  server_thread.join();

  return server.port() > 0 ? 0 : 1;
}

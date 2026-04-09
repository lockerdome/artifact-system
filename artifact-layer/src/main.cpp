#include "absl/status/status.h"
#include "service/server.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <print>
#include <thread>

namespace {
std::atomic<bool> g_shutdown_requested{false};

void SignalHandler(int) {
  g_shutdown_requested.store(true, std::memory_order_relaxed);
}
} // namespace

int main() {
  artifact_system::service::ServerConfig config;

  const char* listen_address = std::getenv("ARTIFACT_LAYER_LISTEN_ADDRESS");
  if (listen_address && listen_address[0] != '\0') {
    config.listen_address = listen_address;
  }

  artifact_system::service::ArtifactLayerServer server(config);

  auto status = server.Initialize();
  if (!status.ok()) {
    std::println(stderr, "Failed to initialize server: {}", std::string(status.message()));
    return 1;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  std::thread server_thread([&server] { server.Start(); });

  while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  server.Shutdown();
  server_thread.join();

  return 0;
}

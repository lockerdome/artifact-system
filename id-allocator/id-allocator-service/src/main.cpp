#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>

#include "config.h"
#include "server.h"
#include "store/datastore_store.h"
#include "store/memory_store.h"

namespace {

// Signal handler sets this flag; a dedicated thread polls it and calls Shutdown.
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int /*signum*/) {
  g_shutdown_requested.store(true, std::memory_order_relaxed);
}

/// Read an environment variable, returning `fallback` if unset or empty.
std::string env_or(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  if (value != nullptr && value[0] != '\0') {
    return value;
  }
  return fallback;
}

/// Resolve the gRPC listen address.
///
/// Cloud Run injects PORT to tell the container which port to serve on. When it
/// is set we bind 0.0.0.0:$PORT so deploys work without any extra address
/// configuration. Otherwise we fall back to ID_ALLOCATOR_LISTEN_ADDRESS, which
/// still lets local/dev runs control the full host:port (default
/// 0.0.0.0:50051).
std::string resolve_listen_address() {
  const char* port = std::getenv("PORT");
  if (port != nullptr && port[0] != '\0') {
    return "0.0.0.0:" + std::string(port);
  }
  return env_or("ID_ALLOCATOR_LISTEN_ADDRESS", "0.0.0.0:50051");
}

id_allocator::ServerConfig load_config() {
  return {
      .listen_address = resolve_listen_address(),
      .store_type = env_or("ID_ALLOCATOR_STORE_TYPE", "memory"),
      .gcp_project_id = env_or("ID_ALLOCATOR_GCP_PROJECT_ID", ""),
      .datastore_endpoint = env_or("ID_ALLOCATOR_DATASTORE_ENDPOINT", ""),
  };
}

std::unique_ptr<id_allocator::BlockStore> create_store(const id_allocator::ServerConfig& config) {

  if (config.store_type == "memory") {
    std::println("Using in-memory store");
    return std::make_unique<id_allocator::MemoryStore>();
  }

  if (config.store_type == "datastore") {
    if (config.gcp_project_id.empty()) {
      std::println(stderr, "ID_ALLOCATOR_GCP_PROJECT_ID is required for datastore store");
      std::exit(1);
    }

    std::string endpoint = config.datastore_endpoint.empty() ? "datastore.googleapis.com:443" : config.datastore_endpoint;

    std::println("Using Datastore store (project={}, endpoint={})", config.gcp_project_id, endpoint);

    auto channel = grpc::CreateChannel(endpoint, grpc::GoogleDefaultCredentials());

    return std::make_unique<id_allocator::DatastoreStore>(config.gcp_project_id, std::move(channel));
  }

  std::println(stderr, "Unknown store type: {}", config.store_type);
  std::exit(1);
}

} // namespace

int main() {
  auto config = load_config();
  auto store = create_store(config);

  id_allocator::Server server{std::move(config), std::move(store)};

  // Install signal handlers for graceful shutdown.
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);

  // Dedicated thread that waits for the shutdown signal and calls Shutdown()
  // outside of the signal handler context (to avoid gRPC internal mutex issues).
  std::jthread shutdown_watcher([&server](std::stop_token) {
    while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.shutdown();
  });

  server.run();

  g_shutdown_requested.store(true, std::memory_order_relaxed);
  // jthread destructor joins the shutdown watcher.

  std::println("Server shut down cleanly.");
  return 0;
}

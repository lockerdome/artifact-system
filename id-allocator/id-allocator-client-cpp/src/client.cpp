#include "client.h"

#include <stdexcept>
#include <utility>

namespace id_allocator::client {

IdAllocatorClient::IdAllocatorClient(IdAllocatorClientOptions options)
    : grpc_client_({
          .service_address = options.service_address,
          .partition_id = options.partition_id,
          .retry = options.retry,
          .channel_credentials = options.channel_credentials,
      }),
      buffer_(options.high_water_mark, [this] { return grpc_client_.fetch_block(); }) {
  if (options.service_address.empty()) {
    throw std::runtime_error("service_address is required");
  }
  if (options.partition_id.empty()) {
    throw std::runtime_error("partition_id is required");
  }
}

void IdAllocatorClient::initialize() {
  if (initialized_) {
    throw std::runtime_error("Client is already initialized");
  }

  grpc_client_.connect();
  buffer_.initialize();
  initialized_ = true;
}

uint64_t IdAllocatorClient::allocate_id() {
  if (!initialized_) {
    throw std::runtime_error("Client is not initialized. Call initialize() first.");
  }

  return buffer_.allocate_id();
}

void IdAllocatorClient::close() {
  grpc_client_.close();
  buffer_.close();
  initialized_ = false;
}

} // namespace id_allocator::client

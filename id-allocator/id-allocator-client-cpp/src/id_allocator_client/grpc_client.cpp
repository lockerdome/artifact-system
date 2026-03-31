#include "id_allocator_client/grpc_client.h"

#include <utility>

#include "id_allocator.pb.h"

namespace id_allocator::client {

RpcError::RpcError(grpc::StatusCode code, std::string message) : std::runtime_error(std::move(message)), code_(code) {
}

grpc::StatusCode RpcError::code() const {
  return code_;
}

IdAllocatorGrpcClient::IdAllocatorGrpcClient(GrpcClientOptions options) : options_(std::move(options)) {
  if (options_.service_address.empty()) {
    throw std::runtime_error("service_address is required");
  }
  if (options_.partition_id.empty()) {
    throw std::runtime_error("partition_id is required");
  }
}

void IdAllocatorGrpcClient::connect() {
  if (connected_) {
    return;
  }

  channel_ = grpc::CreateChannel(options_.service_address, options_.channel_credentials);
  stub_ = id_allocator::IdAllocator::NewStub(channel_);
  connected_ = true;
}

void IdAllocatorGrpcClient::close() {
  stub_.reset();
  channel_.reset();
  connected_ = false;
}

BlockRange IdAllocatorGrpcClient::fetch_block() {
  if (!connected_) {
    throw std::runtime_error("Client is not connected. Call connect() first.");
  }

  return retry_with_backoff([this] { return allocate_block_rpc(); }, options_.retry);
}

BlockRange IdAllocatorGrpcClient::allocate_block_rpc() {
  id_allocator::AllocateBlockRequest request;
  request.set_partition_id(options_.partition_id);

  id_allocator::AllocateBlockResponse response;
  grpc::ClientContext context;
  const grpc::Status status = stub_->AllocateBlock(&context, request, &response);

  if (!status.ok()) {
    throw RpcError(status.error_code(), status.error_message());
  }

  return {
      .range_start = response.range_start(),
      .range_end = response.range_end(),
  };
}

} // namespace id_allocator::client

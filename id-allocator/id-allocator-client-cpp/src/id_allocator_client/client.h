#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "id_allocator_client/block_double_buffer.h"
#include "id_allocator_client/grpc_client.h"

namespace id_allocator::client {

struct IdAllocatorClientOptions {
  std::string service_address;
  std::string partition_id;
  uint64_t high_water_mark = 1000;
  RetryOptions retry;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials = grpc::InsecureChannelCredentials();
};

class IdAllocatorClient {
public:
  explicit IdAllocatorClient(IdAllocatorClientOptions options);

  void initialize();
  [[nodiscard]] uint64_t allocate_id();
  void close();

private:
  IdAllocatorGrpcClient grpc_client_;
  BlockDoubleBuffer buffer_;
  bool initialized_ = false;
};

} // namespace id_allocator::client

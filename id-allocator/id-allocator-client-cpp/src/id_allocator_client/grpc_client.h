#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <grpcpp/grpcpp.h>

#include "id_allocator.grpc.pb.h"
#include "id_allocator_client/retry.h"
#include "id_allocator_client/types.h"

namespace id_allocator::client {

struct GrpcClientOptions {
  std::string service_address;
  std::string partition_id;
  RetryOptions retry;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials = grpc::InsecureChannelCredentials();
};

class RpcError : public std::runtime_error {
public:
  RpcError(grpc::StatusCode code, std::string message);

  [[nodiscard]] grpc::StatusCode code() const;

private:
  grpc::StatusCode code_;
};

class IdAllocatorGrpcClient {
public:
  explicit IdAllocatorGrpcClient(GrpcClientOptions options);

  void connect();
  void close();
  [[nodiscard]] BlockRange fetch_block();

private:
  [[nodiscard]] BlockRange allocate_block_rpc();

  GrpcClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<id_allocator::IdAllocator::Stub> stub_;
  bool connected_ = false;
};

} // namespace id_allocator::client

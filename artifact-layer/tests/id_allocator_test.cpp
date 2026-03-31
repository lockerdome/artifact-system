#include "id/id_allocator.h"

#include <atomic>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "id_allocator.grpc.pb.h"
#include "id_allocator.pb.h"

namespace artifact_system {
namespace {

// Fake gRPC IdAllocator service for testing the production allocator.
class FakeIdAllocatorService final : public id_allocator::IdAllocator::Service {
public:
  grpc::Status AllocateBlock(grpc::ServerContext* /*context*/, const id_allocator::AllocateBlockRequest* /*request*/,
                             id_allocator::AllocateBlockResponse* response) override {
    if (fail_.load()) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "ID service unavailable");
    }
    uint64_t size = block_size_.load();
    uint64_t start = next_id_.fetch_add(size);
    response->set_range_start(start);
    response->set_range_end(start + size);
    return grpc::Status::OK;
  }

  void set_fail(bool fail) {
    fail_.store(fail);
  }
  void set_block_size(uint64_t size) {
    block_size_.store(size);
  }

private:
  std::atomic<uint64_t> next_id_{1};
  std::atomic<bool> fail_{false};
  std::atomic<uint64_t> block_size_ = 100;
};

class IdAllocatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    service_ = std::make_unique<FakeIdAllocatorService>();

    grpc::ServerBuilder builder;
    builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
  }

  std::string address() const {
    return "localhost:" + std::to_string(port_);
  }

  std::unique_ptr<ProductionIdAllocator> MakeAllocator(uint64_t high_water_mark = 10) {
    return std::make_unique<ProductionIdAllocator>(IdAllocatorConfig{
        .service_address = address(),
        .partition_id = "test-partition",
        .high_water_mark = high_water_mark,
        .retry_max_retries = 0,
        .retry_base_delay_ms = 0,
        .retry_max_delay_ms = 0,
    });
  }

  std::unique_ptr<FakeIdAllocatorService> service_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};

TEST_F(IdAllocatorTest, AllocatesUniqueIds) {
  auto allocator = MakeAllocator();
  allocator->Initialize();

  uint64_t id1 = allocator->AllocateId();
  uint64_t id2 = allocator->AllocateId();
  EXPECT_NE(id1, id2);
}

TEST_F(IdAllocatorTest, AllocatesSequentialIds) {
  service_->set_block_size(10);
  auto allocator = MakeAllocator(5);
  allocator->Initialize();

  uint64_t first = allocator->AllocateId();
  for (int i = 1; i < 10; ++i) {
    uint64_t id = allocator->AllocateId();
    EXPECT_EQ(id, first + i);
  }
}

TEST_F(IdAllocatorTest, ThrowsWhenBothBuffersExhausted) {
  service_->set_block_size(2);
  auto allocator = MakeAllocator(1);
  allocator->Initialize(); // Fetches first block: 2 IDs.

  // Make the service fail before any allocation triggers a prefetch.
  service_->set_fail(true);

  // Consume IDs until both buffers are exhausted.
  // With 0 retries, the prefetch fails immediately.
  EXPECT_THROW(
      {
        for (int i = 0; i < 100; ++i) {
          static_cast<void>(allocator->AllocateId());
        }
      },
      std::runtime_error);
}

TEST_F(IdAllocatorTest, ServiceUnavailableOnInitialize) {
  service_->set_fail(true);

  auto allocator = MakeAllocator();
  // Initialize fetches the first block; if the service is down, it should throw.
  EXPECT_THROW(allocator->Initialize(), std::exception);
}

} // namespace
} // namespace artifact_system

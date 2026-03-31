#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "id_allocator_client/client.h"
#include "id_allocator.grpc.pb.h"

namespace id_allocator::client {
namespace {

class MockAllocatorService final : public id_allocator::IdAllocator::Service {
public:
  explicit MockAllocatorService(std::vector<BlockRange> ranges) : ranges_(std::move(ranges)) {
  }

  void set_failures(int failures) {
    fail_next_.store(failures);
  }

  [[nodiscard]] int call_count() const {
    return call_count_.load();
  }

  grpc::Status AllocateBlock(grpc::ServerContext*, const id_allocator::AllocateBlockRequest*, id_allocator::AllocateBlockResponse* response) override {
    ++call_count_;

    int remaining_failures = fail_next_.load();
    while (remaining_failures > 0) {
      if (fail_next_.compare_exchange_weak(remaining_failures, remaining_failures - 1)) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "transient unavailable");
      }
    }

    std::lock_guard lock(mutex_);
    if (cursor_ >= ranges_.size()) {
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "no more test blocks");
    }

    const auto [range_start, range_end] = ranges_[cursor_++];
    response->set_range_start(range_start);
    response->set_range_end(range_end);
    return grpc::Status::OK;
  }

private:
  std::vector<BlockRange> ranges_;
  size_t cursor_ = 0;
  std::atomic<int> fail_next_{0};
  std::atomic<int> call_count_{0};
  std::mutex mutex_;
};

class ClientIntegrationTest : public ::testing::Test {
protected:
  void StartServer(std::vector<BlockRange> ranges, int failures = 0) {
    service_ = std::make_unique<MockAllocatorService>(std::move(ranges));
    service_->set_failures(failures);

    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();

    ASSERT_NE(server_, nullptr);
    ASSERT_GT(port, 0);
    address_ = "localhost:" + std::to_string(port);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
  }

  static bool WaitForCallCountAtLeast(const MockAllocatorService& service, int min_calls, std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    while ((std::chrono::steady_clock::now() - start) < timeout) {
      if (service.call_count() >= min_calls) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return service.call_count() >= min_calls;
  }

  std::unique_ptr<MockAllocatorService> service_;
  std::unique_ptr<grpc::Server> server_;
  std::string address_;
};

TEST_F(ClientIntegrationTest, AllocateIdRequiresInitialize) {
  StartServer({{100, 104}});

  IdAllocatorClient client({
      .service_address = address_,
      .partition_id = "partition-a",
      .high_water_mark = 2,
  });

  EXPECT_THROW((void)client.allocate_id(), std::runtime_error);
}

TEST_F(ClientIntegrationTest, UsesPrefetchedBackBlockAfterFrontExhaustion) {
  StartServer({{100, 104}, {200, 204}});

  IdAllocatorClient client({
      .service_address = address_,
      .partition_id = "partition-a",
      .high_water_mark = 3,
      .retry =
          {
              .max_retries = 2,
              .base_delay_ms = 1,
              .max_delay_ms = 2,
          },
  });
  client.initialize();

  EXPECT_EQ(client.allocate_id(), 100u);
  ASSERT_TRUE(WaitForCallCountAtLeast(*service_, 2, std::chrono::milliseconds(500)));

  EXPECT_EQ(client.allocate_id(), 101u);
  EXPECT_EQ(client.allocate_id(), 102u);
  EXPECT_EQ(client.allocate_id(), 103u);
  EXPECT_EQ(client.allocate_id(), 200u);
}

TEST_F(ClientIntegrationTest, RetriesTransientFailuresDuringInitialize) {
  StartServer({{50, 53}}, 2);

  IdAllocatorClient client({
      .service_address = address_,
      .partition_id = "partition-a",
      .high_water_mark = 2,
      .retry =
          {
              .max_retries = 5,
              .base_delay_ms = 1,
              .max_delay_ms = 2,
          },
  });
  client.initialize();

  EXPECT_GE(service_->call_count(), 3);
  EXPECT_EQ(client.allocate_id(), 50u);
}

TEST_F(ClientIntegrationTest, ThrowsWhenBothBuffersAreExhausted) {
  StartServer({{1, 3}});

  IdAllocatorClient client({
      .service_address = address_,
      .partition_id = "partition-a",
      .high_water_mark = 1,
      .retry =
          {
              .max_retries = 0,
              .base_delay_ms = 1,
              .max_delay_ms = 1,
          },
  });
  client.initialize();

  EXPECT_EQ(client.allocate_id(), 1u);
  EXPECT_EQ(client.allocate_id(), 2u);
  EXPECT_THROW((void)client.allocate_id(), std::runtime_error);
}

} // namespace
} // namespace id_allocator::client

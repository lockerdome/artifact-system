#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include "id_allocator.grpc.pb.h"
#include "id_allocator.pb.h"
#include "partition/partition_manager.h"
#include "service/id_allocator_admin_service.h"
#include "service/id_allocator_service.h"
#include "store/memory_store.h"

namespace id_allocator {
namespace {

// Fixture that spins up an in-process gRPC server with both services.
class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = std::make_unique<MemoryStore>();
        partition_manager_ = std::make_unique<PartitionManager>(*store_);
        ASSERT_TRUE(partition_manager_->initialize().has_value());

        admin_service_ = std::make_unique<IdAllocatorAdminServiceImpl>(
            *partition_manager_);
        allocator_service_ = std::make_unique<IdAllocatorServiceImpl>(
            *partition_manager_);

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort(
            "localhost:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(admin_service_.get());
        builder.RegisterService(allocator_service_.get());
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        ASSERT_GT(port, 0);

        auto channel = grpc::CreateChannel(
            "localhost:" + std::to_string(port),
            grpc::InsecureChannelCredentials());
        admin_stub_ = IdAllocatorAdmin::NewStub(channel);
        allocator_stub_ = IdAllocator::NewStub(channel);
    }

    void TearDown() override {
        if (server_) {
            server_->Shutdown();
        }
    }

    // Helper: create a partition through gRPC with standard test config.
    grpc::Status CreateTestPartition(
            const std::string& partition_id,
            uint32_t block_size = 2048,
            uint32_t super_block_size = 65536) {
        CreatePartitionRequest req;
        req.set_partition_id(partition_id);
        req.set_num_buckets(128);
        req.set_bucket_size_bits(40);
        req.set_super_block_size(super_block_size);
        req.set_block_size(block_size);

        CreatePartitionResponse resp;
        grpc::ClientContext ctx;
        return admin_stub_->CreatePartition(&ctx, req, &resp);
    }

    std::unique_ptr<MemoryStore> store_;
    std::unique_ptr<PartitionManager> partition_manager_;
    std::unique_ptr<IdAllocatorAdminServiceImpl> admin_service_;
    std::unique_ptr<IdAllocatorServiceImpl> allocator_service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<IdAllocatorAdmin::Stub> admin_stub_;
    std::unique_ptr<IdAllocator::Stub> allocator_stub_;
};

// a. CreatePartition with valid config → OK
TEST_F(IntegrationTest, CreatePartitionOk) {
    auto status = CreateTestPartition("my-partition");
    EXPECT_TRUE(status.ok()) << status.error_message();
}

// b. CreatePartition with same ID → ALREADY_EXISTS
TEST_F(IntegrationTest, CreatePartitionDuplicateReturnsAlreadyExists) {
    ASSERT_TRUE(CreateTestPartition("dup").ok());

    auto status = CreateTestPartition("dup");
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

// c. CreatePartition with invalid config → INVALID_ARGUMENT
TEST_F(IntegrationTest, CreatePartitionInvalidConfigReturnsInvalidArgument) {
    CreatePartitionRequest req;
    req.set_partition_id("bad-config");
    req.set_num_buckets(128);
    req.set_bucket_size_bits(40);
    req.set_super_block_size(65536);
    req.set_block_size(0);  // invalid

    CreatePartitionResponse resp;
    grpc::ClientContext ctx;
    auto status = admin_stub_->CreatePartition(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// d. GetPartition → returns correct config
TEST_F(IntegrationTest, GetPartitionReturnsCorrectConfig) {
    ASSERT_TRUE(CreateTestPartition("get-me").ok());

    GetPartitionRequest req;
    req.set_partition_id("get-me");
    GetPartitionResponse resp;
    grpc::ClientContext ctx;
    auto status = admin_stub_->GetPartition(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.partition_id(), "get-me");
    EXPECT_EQ(resp.num_buckets(), 128u);
    EXPECT_EQ(resp.bucket_size_bits(), 40u);
    EXPECT_EQ(resp.super_block_size(), 65536u);
    EXPECT_EQ(resp.block_size(), 2048u);
}

// e. AllocateBlock → returns valid range
TEST_F(IntegrationTest, AllocateBlockReturnsValidRange) {
    ASSERT_TRUE(CreateTestPartition("alloc-test").ok());

    AllocateBlockRequest req;
    req.set_partition_id("alloc-test");
    AllocateBlockResponse resp;
    grpc::ClientContext ctx;
    auto status = allocator_stub_->AllocateBlock(&ctx, req, &resp);

    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_LT(resp.range_start(), resp.range_end());
    EXPECT_EQ(resp.range_end() - resp.range_start(), 2048u);
}

// f. AllocateBlock multiple times → all ranges non-overlapping
TEST_F(IntegrationTest, AllocateBlockMultipleTimesNonOverlapping) {
    ASSERT_TRUE(CreateTestPartition("multi-alloc").ok());

    std::set<uint64_t> all_ids;
    constexpr int kAllocations = 10;

    for (int i = 0; i < kAllocations; ++i) {
        AllocateBlockRequest req;
        req.set_partition_id("multi-alloc");
        AllocateBlockResponse resp;
        grpc::ClientContext ctx;
        auto status = allocator_stub_->AllocateBlock(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << "allocation " << i << ": "
                                 << status.error_message();

        for (uint64_t id = resp.range_start(); id < resp.range_end(); ++id) {
            auto [_, inserted] = all_ids.insert(id);
            EXPECT_TRUE(inserted)
                << "Duplicate ID " << id << " in allocation " << i;
        }
    }

    EXPECT_EQ(all_ids.size(), static_cast<size_t>(kAllocations * 2048));
}

// g. AllocateBlock for non-existent partition → NOT_FOUND
TEST_F(IntegrationTest, AllocateBlockNonExistentPartitionReturnsNotFound) {
    AllocateBlockRequest req;
    req.set_partition_id("no-such-partition");
    AllocateBlockResponse resp;
    grpc::ClientContext ctx;
    auto status = allocator_stub_->AllocateBlock(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// h. DeletePartition → OK
TEST_F(IntegrationTest, DeletePartitionOk) {
    ASSERT_TRUE(CreateTestPartition("delete-me").ok());

    DeletePartitionRequest req;
    req.set_partition_id("delete-me");
    DeletePartitionResponse resp;
    grpc::ClientContext ctx;
    auto status = admin_stub_->DeletePartition(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
}

// i. GetPartition after delete → NOT_FOUND
TEST_F(IntegrationTest, GetPartitionAfterDeleteReturnsNotFound) {
    ASSERT_TRUE(CreateTestPartition("ghost").ok());

    {
        DeletePartitionRequest req;
        req.set_partition_id("ghost");
        DeletePartitionResponse resp;
        grpc::ClientContext ctx;
        ASSERT_TRUE(admin_stub_->DeletePartition(&ctx, req, &resp).ok());
    }

    GetPartitionRequest req;
    req.set_partition_id("ghost");
    GetPartitionResponse resp;
    grpc::ClientContext ctx;
    auto status = admin_stub_->GetPartition(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

// j. AllocateBlock after delete → NOT_FOUND
TEST_F(IntegrationTest, AllocateBlockAfterDeleteReturnsNotFound) {
    ASSERT_TRUE(CreateTestPartition("soon-gone").ok());

    {
        DeletePartitionRequest req;
        req.set_partition_id("soon-gone");
        DeletePartitionResponse resp;
        grpc::ClientContext ctx;
        ASSERT_TRUE(admin_stub_->DeletePartition(&ctx, req, &resp).ok());
    }

    AllocateBlockRequest req;
    req.set_partition_id("soon-gone");
    AllocateBlockResponse resp;
    grpc::ClientContext ctx;
    auto status = allocator_stub_->AllocateBlock(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

}  // namespace
}  // namespace id_allocator

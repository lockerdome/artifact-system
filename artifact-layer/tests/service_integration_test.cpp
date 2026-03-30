#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "google/protobuf/any.pb.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/wire_format_lite.h"
#include "grpcpp/grpcpp.h"
#include "gtest/gtest.h"

#include "absl/strings/str_cat.h"
#include "artifact/proto_utils.h"
#include "artifact_service.grpc.pb.h"
#include "artifact_service.pb.h"
#include "service/server.h"

namespace artifact_system::testing {
namespace {

// ---------------------------------------------------------------------------
// Proto source for a test type with an index.
// ---------------------------------------------------------------------------

constexpr const char* kTestProtoSource = R"(
syntax = "proto3";
import "artifact_options.proto";
message TestArtifact {
  option (artifact_system.indexes) = {
    key_type: "TestArtifact_by_name"
    key: "name"
    order: { field: "artifact_id" direction: ASCENDING }
  };
  string name = 1;
  uint64 artifact_id = 2;
}
)";

constexpr const char* kTestProtoSourceV2 = R"(
syntax = "proto3";
import "artifact_options.proto";
message TestArtifact {
  option (artifact_system.indexes) = {
    key_type: "TestArtifact_by_name"
    key: "name"
    order: { field: "artifact_id" direction: ASCENDING }
  };
  string name = 1;
  uint64 artifact_id = 2;
  string description = 3;
}
)";

// ---------------------------------------------------------------------------
// Helper: build a simple TestArtifact payload using proto3 wire format.
// field 1 (string, tag 0x0a): name
// ---------------------------------------------------------------------------

std::string MakeTestPayload(const std::string& name) {
  std::string payload;
  // field 1, wire type 2 (length-delimited) => tag = (1 << 3) | 2 = 0x0a
  payload.push_back(0x0a);
  payload.push_back(static_cast<char>(name.size()));
  payload.append(name);
  return payload;
}

// ---------------------------------------------------------------------------
// Helper: extract the first error detail proto from gRPC binary error_details.
//
// The binary error_details is a serialized google.rpc.Status:
//   field 1 (int32): code
//   field 2 (string): message
//   field 3 (repeated google.protobuf.Any): details
//
// We look for the first field-3 entry whose type_url matches DetailProto,
// then parse the Any's value bytes into the output.
// ---------------------------------------------------------------------------

template <typename DetailProto> bool ExtractErrorDetail(const grpc::Status& status, DetailProto* detail) {
  const std::string& error_details = status.error_details();
  if (error_details.empty())
    return false;

  const std::string expected_type_url = absl::StrCat("type.googleapis.com/", DetailProto::descriptor()->full_name());

  google::protobuf::io::CodedInputStream stream(reinterpret_cast<const uint8_t*>(error_details.data()), static_cast<int>(error_details.size()));

  uint32_t tag;
  while ((tag = stream.ReadTag()) != 0) {
    int field_number = google::protobuf::internal::WireFormatLite::GetTagFieldNumber(tag);
    auto wire_type = google::protobuf::internal::WireFormatLite::GetTagWireType(tag);

    if (field_number == 3 && wire_type == google::protobuf::internal::WireFormatLite::WIRETYPE_LENGTH_DELIMITED) {
      // Read the embedded Any message.
      uint32_t len;
      stream.ReadVarint32(&len);
      std::string any_bytes;
      stream.ReadString(&any_bytes, len);

      google::protobuf::Any any;
      if (any.ParseFromString(any_bytes) && any.type_url() == expected_type_url) {
        return detail->ParseFromString(any.value());
      }
    } else {
      google::protobuf::internal::WireFormatLite::SkipField(&stream, tag);
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Test fixture: in-process gRPC server with MemoryStorage.
// ---------------------------------------------------------------------------

class ServiceIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    service::ServerConfig config;
    config.listen_address = "localhost:0";
    server_ = std::make_unique<service::ArtifactLayerServer>(config);
    ASSERT_TRUE(server_->Initialize().ok());

    // Start() blocks, so run it on a background thread.
    server_thread_ = std::thread([this] { server_->Start(); });

    // Wait for the port to become available.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto channel = grpc::CreateChannel("localhost:" + std::to_string(server_->port()), grpc::InsecureChannelCredentials());

    snapshot_txn_stub_ = SnapshotTransactionService::NewStub(channel);
    artifact_stub_ = ArtifactService::NewStub(channel);
    index_stub_ = IndexService::NewStub(channel);
    type_registry_stub_ = TypeRegistryService::NewStub(channel);
  }

  void TearDown() override {
    server_->Shutdown();
    if (server_thread_.joinable())
      server_thread_.join();
  }

  // ---- Convenience helpers ------------------------------------------------

  // Register the default test type and return its version_id.
  uint64_t RegisterTestType() {
    grpc::ClientContext ctx;
    RegisterTypeVersionRequest req;
    req.set_type_name("TestArtifact");
    req.set_proto_source(kTestProtoSource);
    RegisterTypeVersionResponse resp;
    auto status = type_registry_stub_->RegisterTypeVersion(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return resp.version_id();
  }

  // Create an artifact with the given version_id and name payload.
  // Returns (artifact_id, snapshot_id).
  std::pair<uint64_t, std::string> CreateTestArtifact(uint64_t version_id, const std::string& name, std::optional<std::string> transaction_id = std::nullopt) {
    grpc::ClientContext ctx;
    CreateArtifactRequest req;
    req.set_version_id(version_id);
    req.set_payload(MakeTestPayload(name));
    if (transaction_id.has_value()) {
      req.set_transaction_id(*transaction_id);
    }
    CreateArtifactResponse resp;
    auto status = artifact_stub_->CreateArtifact(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return {resp.artifact_id(), resp.snapshot_id()};
  }

  // Create a transaction (forking from canonical head) and return its id.
  std::string CreateTransaction() {
    grpc::ClientContext ctx;
    CreateTransactionRequest req;
    CreateTransactionResponse resp;
    auto status = snapshot_txn_stub_->CreateTransaction(&ctx, req, &resp);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return resp.transaction_id();
  }

  std::unique_ptr<service::ArtifactLayerServer> server_;
  std::thread server_thread_;
  std::unique_ptr<SnapshotTransactionService::Stub> snapshot_txn_stub_;
  std::unique_ptr<ArtifactService::Stub> artifact_stub_;
  std::unique_ptr<IndexService::Stub> index_stub_;
  std::unique_ptr<TypeRegistryService::Stub> type_registry_stub_;
};

// ===========================================================================
// TypeRegistryService tests
// ===========================================================================

TEST_F(ServiceIntegrationTest, RegisterTypeVersion_Success) {
  uint64_t version_id = RegisterTestType();
  EXPECT_GT(version_id, 0u);
}

TEST_F(ServiceIntegrationTest, RegisterTypeVersion_InvalidProto) {
  grpc::ClientContext ctx;
  RegisterTypeVersionRequest req;
  req.set_type_name("BadType");
  req.set_proto_source("this is not valid proto");
  RegisterTypeVersionResponse resp;
  auto status = type_registry_stub_->RegisterTypeVersion(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::INVALID_ARGUMENT);

  RegisterTypeVersionError detail;
  EXPECT_TRUE(ExtractErrorDetail(status, &detail));
  EXPECT_GT(detail.violations_size(), 0);
}

TEST_F(ServiceIntegrationTest, GetTypeVersion_Success) {
  uint64_t version_id = RegisterTestType();

  grpc::ClientContext ctx;
  GetTypeVersionRequest req;
  req.set_version_id(version_id);
  GetTypeVersionResponse resp;
  auto status = type_registry_stub_->GetTypeVersion(&ctx, req, &resp);
  ASSERT_TRUE(status.ok()) << status.error_message();

  EXPECT_EQ(resp.version_id(), version_id);
  EXPECT_GT(resp.type_id(), 0u);
  EXPECT_FALSE(resp.proto_source().empty());
  EXPECT_GT(resp.descriptor_set().file_size(), 0);
}

TEST_F(ServiceIntegrationTest, GetTypeVersion_NotFound) {
  grpc::ClientContext ctx;
  GetTypeVersionRequest req;
  req.set_version_id(999999);
  GetTypeVersionResponse resp;
  auto status = type_registry_stub_->GetTypeVersion(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

TEST_F(ServiceIntegrationTest, ListTypeVersions_Success) {
  // Register two versions of the same type.
  uint64_t v1 = RegisterTestType();

  grpc::ClientContext ctx2;
  RegisterTypeVersionRequest req2;
  req2.set_type_name("TestArtifact");
  req2.set_proto_source(kTestProtoSourceV2);
  RegisterTypeVersionResponse resp2;
  auto s2 = type_registry_stub_->RegisterTypeVersion(&ctx2, req2, &resp2);
  ASSERT_TRUE(s2.ok()) << s2.error_message();
  uint64_t v2 = resp2.version_id();

  EXPECT_NE(v1, v2);

  grpc::ClientContext ctx3;
  ListTypeVersionsRequest list_req;
  list_req.set_type_name("TestArtifact");
  ListTypeVersionsResponse list_resp;
  auto s3 = type_registry_stub_->ListTypeVersions(&ctx3, list_req, &list_resp);
  ASSERT_TRUE(s3.ok()) << s3.error_message();

  ASSERT_GE(list_resp.version_ids_size(), 2);
  // Both version IDs should be present.
  std::vector<uint64_t> ids(list_resp.version_ids().begin(), list_resp.version_ids().end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), v1), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), v2), ids.end());
}

TEST_F(ServiceIntegrationTest, ListTypeVersions_NotFound) {
  grpc::ClientContext ctx;
  ListTypeVersionsRequest req;
  req.set_type_name("NonExistentType");
  ListTypeVersionsResponse resp;
  auto status = type_registry_stub_->ListTypeVersions(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

TEST_F(ServiceIntegrationTest, GetIndexSchema_Success) {
  RegisterTestType();

  grpc::ClientContext ctx;
  GetIndexSchemaRequest req;
  req.set_key_type("TestArtifact_by_name");
  GetIndexSchemaResponse resp;
  auto status = type_registry_stub_->GetIndexSchema(&ctx, req, &resp);
  ASSERT_TRUE(status.ok()) << status.error_message();

  EXPECT_EQ(resp.key_type(), "TestArtifact_by_name");
  EXPECT_GT(resp.index_definition_id(), 0u);
  EXPECT_GT(resp.key_fields_size(), 0);
  EXPECT_FALSE(resp.key_message_name().empty());
  EXPECT_FALSE(resp.value_message_name().empty());
  EXPECT_FALSE(resp.index_message_name().empty());
  EXPECT_GT(resp.index_descriptor_set().file_size(), 0);
}

TEST_F(ServiceIntegrationTest, GetIndexSchema_NotFound) {
  grpc::ClientContext ctx;
  GetIndexSchemaRequest req;
  req.set_key_type("NonExistentIndex");
  GetIndexSchemaResponse resp;
  auto status = type_registry_stub_->GetIndexSchema(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

// ===========================================================================
// ArtifactService tests
// ===========================================================================

TEST_F(ServiceIntegrationTest, CreateArtifact_Success) {
  uint64_t version_id = RegisterTestType();
  auto [artifact_id, snapshot_id] = CreateTestArtifact(version_id, "test1");

  EXPECT_GT(artifact_id, 0u);
  EXPECT_FALSE(snapshot_id.empty());
}

TEST_F(ServiceIntegrationTest, CreateArtifact_InvalidVersionId) {
  grpc::ClientContext ctx;
  CreateArtifactRequest req;
  req.set_version_id(999999);
  req.set_payload(MakeTestPayload("test"));
  CreateArtifactResponse resp;
  auto status = artifact_stub_->CreateArtifact(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::INVALID_ARGUMENT);
}

TEST_F(ServiceIntegrationTest, GetArtifact_Success) {
  uint64_t version_id = RegisterTestType();
  auto [artifact_id, snapshot_id] = CreateTestArtifact(version_id, "get_me");

  grpc::ClientContext ctx;
  GetArtifactRequest req;
  req.set_artifact_id(artifact_id);
  GetArtifactResponse resp;
  auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
  ASSERT_TRUE(status.ok()) << status.error_message();

  EXPECT_EQ(resp.artifact_id(), artifact_id);
  EXPECT_EQ(resp.type_name(), "TestArtifact");
  EXPECT_EQ(resp.version_id(), version_id);
  EXPECT_EQ(resp.payload(), MakeTestPayload("get_me"));
}

TEST_F(ServiceIntegrationTest, GetArtifact_NotFound) {
  grpc::ClientContext ctx;
  GetArtifactRequest req;
  req.set_artifact_id(999999);
  GetArtifactResponse resp;
  auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

TEST_F(ServiceIntegrationTest, BatchGetArtifacts_MixedResults) {
  uint64_t version_id = RegisterTestType();
  auto [id1, _s1] = CreateTestArtifact(version_id, "batch1");
  auto [id2, _s2] = CreateTestArtifact(version_id, "batch2");
  uint64_t nonexistent_id = 999999;

  grpc::ClientContext ctx;
  BatchGetArtifactsRequest req;
  req.add_artifact_ids(id1);
  req.add_artifact_ids(id2);
  req.add_artifact_ids(nonexistent_id);
  BatchGetArtifactsResponse resp;
  auto status = artifact_stub_->BatchGetArtifacts(&ctx, req, &resp);
  ASSERT_TRUE(status.ok()) << status.error_message();

  ASSERT_EQ(resp.results_size(), 3);

  // First two should be found.
  EXPECT_TRUE(resp.results(0).has_artifact());
  EXPECT_EQ(resp.results(0).artifact().artifact_id(), id1);
  EXPECT_TRUE(resp.results(1).has_artifact());
  EXPECT_EQ(resp.results(1).artifact().artifact_id(), id2);

  // Third should be not_found.
  EXPECT_TRUE(resp.results(2).has_not_found());
  EXPECT_EQ(resp.results(2).not_found().artifact_id(), nonexistent_id);
}

TEST_F(ServiceIntegrationTest, UpdateArtifact_Success) {
  uint64_t version_id = RegisterTestType();
  auto [artifact_id, _] = CreateTestArtifact(version_id, "original");

  // Update the artifact.
  {
    grpc::ClientContext ctx;
    UpdateArtifactRequest req;
    req.set_artifact_id(artifact_id);
    req.set_version_id(version_id);
    req.set_payload(MakeTestPayload("updated"));
    UpdateArtifactResponse resp;
    auto status = artifact_stub_->UpdateArtifact(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(resp.snapshot_id().empty());
  }

  // Get and verify updated payload.
  {
    grpc::ClientContext ctx;
    GetArtifactRequest req;
    req.set_artifact_id(artifact_id);
    GetArtifactResponse resp;
    auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.payload(), MakeTestPayload("updated"));
  }
}

TEST_F(ServiceIntegrationTest, DeleteArtifact_Success) {
  uint64_t version_id = RegisterTestType();
  auto [artifact_id, _] = CreateTestArtifact(version_id, "delete_me");

  // Delete.
  {
    grpc::ClientContext ctx;
    DeleteArtifactRequest req;
    req.set_artifact_id(artifact_id);
    DeleteArtifactResponse resp;
    auto status = artifact_stub_->DeleteArtifact(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(resp.snapshot_id().empty());
  }

  // Get should return NOT_FOUND with tombstoned = true.
  {
    grpc::ClientContext ctx;
    GetArtifactRequest req;
    req.set_artifact_id(artifact_id);
    GetArtifactResponse resp;
    auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);

    ArtifactNotFoundError detail;
    if (ExtractErrorDetail(status, &detail)) {
      EXPECT_TRUE(detail.tombstoned());
      EXPECT_EQ(detail.artifact_id(), artifact_id);
    }
  }
}

// ===========================================================================
// SnapshotTransactionService tests
// ===========================================================================

TEST_F(ServiceIntegrationTest, TransactionLifecycle) {
  uint64_t version_id = RegisterTestType();

  // Create a transaction.
  std::string txn_id = CreateTransaction();
  EXPECT_FALSE(txn_id.empty());

  // Create an artifact within the transaction.
  auto [artifact_id, _] = CreateTestArtifact(version_id, "txn_test", txn_id);
  EXPECT_GT(artifact_id, 0u);

  // Commit the transaction.
  std::string commit_snapshot_id;
  {
    grpc::ClientContext ctx;
    CommitTransactionRequest req;
    req.set_transaction_id(txn_id);
    CommitTransactionResponse resp;
    auto status = snapshot_txn_stub_->CommitTransaction(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    commit_snapshot_id = resp.snapshot_id();
    EXPECT_FALSE(commit_snapshot_id.empty());
  }

  // Verify the artifact is visible after commit (default read context).
  {
    grpc::ClientContext ctx;
    GetArtifactRequest req;
    req.set_artifact_id(artifact_id);
    GetArtifactResponse resp;
    auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(resp.artifact_id(), artifact_id);
  }
}

TEST_F(ServiceIntegrationTest, TransactionRollback) {
  uint64_t version_id = RegisterTestType();

  // Create a transaction and an artifact within it.
  std::string txn_id = CreateTransaction();
  auto [artifact_id, _] = CreateTestArtifact(version_id, "rollback_test", txn_id);

  // Rollback.
  {
    grpc::ClientContext ctx;
    RollbackTransactionRequest req;
    req.set_transaction_id(txn_id);
    RollbackTransactionResponse resp;
    auto status = snapshot_txn_stub_->RollbackTransaction(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // The artifact should NOT be visible from the canonical branch.
  {
    grpc::ClientContext ctx;
    GetArtifactRequest req;
    req.set_artifact_id(artifact_id);
    GetArtifactResponse resp;
    auto status = artifact_stub_->GetArtifact(&ctx, req, &resp);
    EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
  }
}

TEST_F(ServiceIntegrationTest, CommitTransaction_NotFound) {
  grpc::ClientContext ctx;
  CommitTransactionRequest req;
  req.set_transaction_id("nonexistent_txn_id");
  CommitTransactionResponse resp;
  auto status = snapshot_txn_stub_->CommitTransaction(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);

  SnapshotTransactionError detail;
  EXPECT_TRUE(ExtractErrorDetail(status, &detail));
  EXPECT_EQ(detail.category(), SnapshotTransactionError::TRANSACTION_NOT_FOUND);
}

TEST_F(ServiceIntegrationTest, CreateSnapshot_Success) {
  grpc::ClientContext ctx;
  CreateSnapshotRequest req;
  // No parent: snapshot the canonical branch head.
  CreateSnapshotResponse resp;
  auto status = snapshot_txn_stub_->CreateSnapshot(&ctx, req, &resp);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(resp.snapshot_id().empty());
}

TEST_F(ServiceIntegrationTest, CreateSnapshot_InvalidParent) {
  grpc::ClientContext ctx;
  CreateSnapshotRequest req;
  req.set_parent_transaction_id("invalid_parent_txn");
  CreateSnapshotResponse resp;
  auto status = snapshot_txn_stub_->CreateSnapshot(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

// ===========================================================================
// IndexService tests
// ===========================================================================

TEST_F(ServiceIntegrationTest, FetchIndex_Success) {
  uint64_t version_id = RegisterTestType();

  // Create an artifact so that the index gets populated.
  CreateTestArtifact(version_id, "indexed_name");

  // Get the index schema to learn the key message format.
  GetIndexSchemaResponse schema;
  {
    grpc::ClientContext ctx;
    GetIndexSchemaRequest req;
    req.set_key_type("TestArtifact_by_name");
    auto status = type_registry_stub_->GetIndexSchema(&ctx, req, &schema);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Build a key using the descriptor set from the schema.
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* key_desc = artifact::BuildPoolAndFindMessage(schema.index_descriptor_set(), schema.key_message_name(), &pool);
  ASSERT_NE(key_desc, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> key_msg(factory.GetPrototype(key_desc)->New());
  const google::protobuf::Reflection* refl = key_msg->GetReflection();
  const google::protobuf::FieldDescriptor* name_field = key_desc->FindFieldByName("name");
  ASSERT_NE(name_field, nullptr);
  refl->SetString(key_msg.get(), name_field, "indexed_name");

  std::string serialized_key = key_msg->SerializeAsString();

  // Fetch the index.
  {
    grpc::ClientContext ctx;
    FetchIndexRequest req;
    req.set_key_type("TestArtifact_by_name");
    req.set_key(serialized_key);
    FetchIndexResponse resp;
    auto status = index_stub_->FetchIndex(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_FALSE(resp.index_payload().empty());
    EXPECT_FALSE(resp.index_message_name().empty());
  }
}

TEST_F(ServiceIntegrationTest, FetchIndex_IndexNotFound) {
  grpc::ClientContext ctx;
  FetchIndexRequest req;
  req.set_key_type("NonExistentIndex");
  req.set_key("anything");
  FetchIndexResponse resp;
  auto status = index_stub_->FetchIndex(&ctx, req, &resp);
  EXPECT_EQ(status.error_code(), grpc::NOT_FOUND);
}

TEST_F(ServiceIntegrationTest, FetchIndex_EmptyResult) {
  uint64_t version_id = RegisterTestType();

  // Create at least one artifact so that the index infrastructure is set up.
  CreateTestArtifact(version_id, "some_name");

  // Get the index schema.
  GetIndexSchemaResponse schema;
  {
    grpc::ClientContext ctx;
    GetIndexSchemaRequest req;
    req.set_key_type("TestArtifact_by_name");
    auto status = type_registry_stub_->GetIndexSchema(&ctx, req, &schema);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }

  // Build a key for a name that has no matching artifacts.
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* key_desc = artifact::BuildPoolAndFindMessage(schema.index_descriptor_set(), schema.key_message_name(), &pool);
  ASSERT_NE(key_desc, nullptr);

  google::protobuf::DynamicMessageFactory factory;
  std::unique_ptr<google::protobuf::Message> key_msg(factory.GetPrototype(key_desc)->New());
  const google::protobuf::Reflection* refl = key_msg->GetReflection();
  const google::protobuf::FieldDescriptor* name_field = key_desc->FindFieldByName("name");
  ASSERT_NE(name_field, nullptr);
  refl->SetString(key_msg.get(), name_field, "no_such_name_exists");

  std::string serialized_key = key_msg->SerializeAsString();

  // Fetch the index -- should return OK with empty index payload.
  {
    grpc::ClientContext ctx;
    FetchIndexRequest req;
    req.set_key_type("TestArtifact_by_name");
    req.set_key(serialized_key);
    FetchIndexResponse resp;
    auto status = index_stub_->FetchIndex(&ctx, req, &resp);
    ASSERT_TRUE(status.ok()) << status.error_message();

    // The response should have the index_message_name set but the payload
    // should be an empty (default) index message.
    EXPECT_FALSE(resp.index_message_name().empty());
  }
}

} // namespace
} // namespace artifact_system::testing

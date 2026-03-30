#include "bootstrap/genesis.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "absl/status/statusor.h"
#include "artifact_internal.pb.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"
#include "id/id_allocator_interface.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "registry/type_registry.h"
#include "storage/memory_storage.h"
#include "transaction/transaction_manager.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

using artifact_system::bootstrap::GenesisIds;
using artifact_system::bootstrap::GenesisResult;
using artifact_system::registry::TypeRegistry;
using artifact_system::transaction::TransactionManager;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

StoredArtifact ReadArtifact(MemoryStorage& storage, uint64_t id) {
  std::string branch = storage.GetCanonicalBranch();
  auto data = storage.GetObject(branch, encoding::ArtifactPath(id));
  EXPECT_TRUE(data.ok()) << "artifact " << id << ": " << data.status();
  StoredArtifact envelope;
  EXPECT_TRUE(envelope.ParseFromString(*data));
  return envelope;
}

std::vector<uint8_t> EncodeStringKey(const google::protobuf::Descriptor& desc, const std::string& field_name, const std::string& value) {
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(&desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  const auto* field = desc.FindFieldByName(field_name);
  msg->GetReflection()->SetString(msg.get(), field, value);
  std::vector<std::string> key_fields = {field_name};
  auto encoded = encoding::EncodeKey(desc, *msg, key_fields);
  EXPECT_TRUE(encoded.ok()) << encoded.status();
  return *encoded;
}

std::vector<uint8_t> EncodeUint64Key(const google::protobuf::Descriptor& desc, const std::string& field_name, uint64_t value) {
  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(&desc);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  const auto* field = desc.FindFieldByName(field_name);
  msg->GetReflection()->SetUInt64(msg.get(), field, value);
  std::vector<std::string> key_fields = {field_name};
  auto encoded = encoding::EncodeKey(desc, *msg, key_fields);
  EXPECT_TRUE(encoded.ok()) << encoded.status();
  return *encoded;
}

index::IndexObject ReadIndexObject(MemoryStorage& storage, uint64_t index_def_id, std::span<const uint8_t> encoded_key, const IndexDefinition& idx_def,
                                   const google::protobuf::Descriptor& parent_desc) {
  std::string branch = storage.GetCanonicalBranch();
  std::string path = encoding::IndexPath(index_def_id, encoded_key);
  auto data = storage.GetObject(branch, path);
  EXPECT_TRUE(data.ok()) << "index " << index_def_id << ": " << data.status();
  auto schema = index::GenerateIndexSchema(idx_def, parent_desc);
  EXPECT_TRUE(schema.ok()) << schema.status();
  auto obj = index::DeserializeIndexObject(*schema, idx_def, *data);
  EXPECT_TRUE(obj.ok()) << obj.status();
  return *obj;
}

std::unordered_set<uint64_t> CollectArtifactIds(const index::IndexObject& obj) {
  std::unordered_set<uint64_t> ids;
  for (const auto& row : obj.rows) {
    ids.insert(row.artifact_id);
  }
  return ids;
}

IndexDefinition FindIndexDef(const google::protobuf::Descriptor& desc, const std::string& key_type) {
  const auto& options = desc.options();
  for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
    const auto& def = options.GetExtension(artifact_system::indexes, i);
    if (def.key_type() == key_type)
      return def;
  }
  ADD_FAILURE() << "index def not found: " << key_type;
  return {};
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class GenesisTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = bootstrap::RunGenesis(&storage_);
    ASSERT_TRUE(result.ok()) << result.status();
    genesis_result_ = std::move(*result);
  }

  MemoryStorage storage_;
  GenesisResult genesis_result_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(GenesisTest, GenesisCreatesAllArtifacts) {
  std::string branch = storage_.GetCanonicalBranch();
  for (uint64_t id = 1; id < GenesisIds::kFirstUserAllocatableId; ++id) {
    auto exists = storage_.ObjectExists(branch, encoding::ArtifactPath(id));
    ASSERT_TRUE(exists.ok()) << exists.status();
    EXPECT_TRUE(*exists) << "artifact " << id << " should exist";
  }
}

TEST_F(GenesisTest, TypeDefinitionArtifactsAreCorrect) {
  struct Expected {
    uint64_t id;
    std::string type_name;
    uint64_t current_version_id;
  };
  const Expected cases[] = {
      {GenesisIds::kIndexDefinitionTypeDef, "artifact_system.IndexDefinition", GenesisIds::kIndexDefinitionTypeVersionDef},
      {GenesisIds::kTypeDefinitionTypeDef, "artifact_system.TypeDefinition", GenesisIds::kTypeDefinitionTypeVersionDef},
      {GenesisIds::kTypeVersionDefinitionTypeDef, "artifact_system.TypeVersionDefinition", GenesisIds::kTypeVersionDefinitionTypeVersionDef},
      {GenesisIds::kReferenceDefinitionTypeDef, "artifact_system.ReferenceDefinition", GenesisIds::kReferenceDefinitionTypeVersionDef},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.type_name);
    auto envelope = ReadArtifact(storage_, c.id);
    EXPECT_EQ(envelope.type_name(), "artifact_system.TypeDefinition");

    TypeDefinition td;
    ASSERT_TRUE(td.ParseFromString(envelope.payload()));
    EXPECT_EQ(td.type_name(), c.type_name);
    EXPECT_EQ(td.current_version_id(), c.current_version_id);
    EXPECT_TRUE(td.deny_create());
    EXPECT_TRUE(td.deny_update());
    EXPECT_TRUE(td.deny_delete());
  }
}

TEST_F(GenesisTest, TypeVersionDefinitionArtifactsAreCorrect) {
  struct Expected {
    uint64_t id;
    uint64_t type_id;
  };
  const Expected cases[] = {
      {GenesisIds::kIndexDefinitionTypeVersionDef, GenesisIds::kIndexDefinitionTypeDef},
      {GenesisIds::kTypeDefinitionTypeVersionDef, GenesisIds::kTypeDefinitionTypeDef},
      {GenesisIds::kTypeVersionDefinitionTypeVersionDef, GenesisIds::kTypeVersionDefinitionTypeDef},
      {GenesisIds::kReferenceDefinitionTypeVersionDef, GenesisIds::kReferenceDefinitionTypeDef},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.id);
    auto envelope = ReadArtifact(storage_, c.id);
    EXPECT_EQ(envelope.type_name(), "artifact_system.TypeVersionDefinition");

    TypeVersionDefinition tvd;
    ASSERT_TRUE(tvd.ParseFromString(envelope.payload()));
    EXPECT_EQ(tvd.type_id(), c.type_id);
    EXPECT_GT(tvd.descriptor_set().file_size(), 0);
    EXPECT_FALSE(tvd.has_previous_version_id());
    EXPECT_FALSE(tvd.has_next_version_id());
  }
}

TEST_F(GenesisTest, IndexDefinitionArtifactsAreCorrect) {
  struct Expected {
    uint64_t id;
    std::string key_type;
  };
  const Expected cases[] = {
      {GenesisIds::kIndexKeyTypeUnique, "index_key_type_unique"},
      {GenesisIds::kAllIndexDefinitions, "all_index_definitions"},
      {GenesisIds::kTypeNameUnique, "type_name_unique"},
      {GenesisIds::kAllTypes, "all_types"},
      {GenesisIds::kTypeVersionsByType, "type_versions_by_type"},
      {GenesisIds::kReferenceKeyTypeUnique, "reference_key_type_unique"},
      {GenesisIds::kReferencesByTargetType, "references_by_target_type"},
      {GenesisIds::kAllReferenceDefinitions, "all_reference_definitions"},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.key_type);
    auto envelope = ReadArtifact(storage_, c.id);
    EXPECT_EQ(envelope.type_name(), "artifact_system.IndexDefinition");

    IndexDefinition idx;
    ASSERT_TRUE(idx.ParseFromString(envelope.payload()));
    EXPECT_EQ(idx.key_type(), c.key_type);
  }
}

TEST_F(GenesisTest, ReferenceDefinitionArtifactIsCorrect) {
  auto envelope = ReadArtifact(storage_, GenesisIds::kRefTypeVersionDefTypeId);
  EXPECT_EQ(envelope.type_name(), "artifact_system.ReferenceDefinition");

  ReferenceDefinition ref;
  ASSERT_TRUE(ref.ParseFromString(envelope.payload()));
  EXPECT_EQ(ref.key_type(), "artifact_system.TypeVersionDefinition.type_id");
  EXPECT_EQ(ref.target_type_name(), "artifact_system.TypeDefinition");
  EXPECT_EQ(ref.referencing_type_name(), "artifact_system.TypeVersionDefinition");
  EXPECT_EQ(ref.field_name(), "type_id");
  EXPECT_EQ(ref.covering_index_key_type(), "type_versions_by_type");
  EXPECT_EQ(ref.on_delete(), ReferenceOption::RESTRICT);
}

TEST_F(GenesisTest, IndexDefIdsMapIsCorrect) {
  const auto& m = genesis_result_.index_def_ids_by_key_type;
  EXPECT_EQ(m.size(), 8);
  EXPECT_EQ(m.at("index_key_type_unique"), GenesisIds::kIndexKeyTypeUnique);
  EXPECT_EQ(m.at("all_index_definitions"), GenesisIds::kAllIndexDefinitions);
  EXPECT_EQ(m.at("type_name_unique"), GenesisIds::kTypeNameUnique);
  EXPECT_EQ(m.at("all_types"), GenesisIds::kAllTypes);
  EXPECT_EQ(m.at("type_versions_by_type"), GenesisIds::kTypeVersionsByType);
  EXPECT_EQ(m.at("reference_key_type_unique"), GenesisIds::kReferenceKeyTypeUnique);
  EXPECT_EQ(m.at("references_by_target_type"), GenesisIds::kReferencesByTargetType);
  EXPECT_EQ(m.at("all_reference_definitions"), GenesisIds::kAllReferenceDefinitions);
}

TEST_F(GenesisTest, GenesisIsIdempotent) {
  auto head_before = storage_.GetBranchHead(storage_.GetCanonicalBranch());
  ASSERT_TRUE(head_before.ok()) << head_before.status();

  auto result2 = bootstrap::RunGenesis(&storage_);
  ASSERT_TRUE(result2.ok()) << result2.status();
  EXPECT_EQ(result2->index_def_ids_by_key_type, genesis_result_.index_def_ids_by_key_type);

  auto head_after = storage_.GetBranchHead(storage_.GetCanonicalBranch());
  ASSERT_TRUE(head_after.ok()) << head_after.status();
  EXPECT_EQ(*head_before, *head_after) << "idempotent genesis should not create new commits";
}

TEST_F(GenesisTest, AllBootstrapIndexesAreQueryable) {
  const auto* td_desc = TypeDefinition::descriptor();
  const auto* idx_desc = IndexDefinition::descriptor();
  const auto* tvd_desc = TypeVersionDefinition::descriptor();
  const auto* rd_desc = ReferenceDefinition::descriptor();

  // type_name_unique: look up each of the 4 type names
  {
    auto idx_def = FindIndexDef(*td_desc, "type_name_unique");
    struct Case {
      std::string name;
      uint64_t expected_id;
    };
    const Case cases[] = {
        {"artifact_system.IndexDefinition", GenesisIds::kIndexDefinitionTypeDef},
        {"artifact_system.TypeDefinition", GenesisIds::kTypeDefinitionTypeDef},
        {"artifact_system.TypeVersionDefinition", GenesisIds::kTypeVersionDefinitionTypeDef},
        {"artifact_system.ReferenceDefinition", GenesisIds::kReferenceDefinitionTypeDef},
    };
    for (const auto& c : cases) {
      SCOPED_TRACE(c.name);
      auto key = EncodeStringKey(*td_desc, "type_name", c.name);
      auto obj = ReadIndexObject(storage_, GenesisIds::kTypeNameUnique, key, idx_def, *td_desc);
      ASSERT_EQ(obj.rows.size(), 1);
      EXPECT_EQ(obj.rows[0].artifact_id, c.expected_id);
    }
  }

  // all_types: contains all 4 TypeDefinition artifact_ids
  {
    auto idx_def = FindIndexDef(*td_desc, "all_types");
    std::vector<uint8_t> empty_key;
    auto obj = ReadIndexObject(storage_, GenesisIds::kAllTypes, empty_key, idx_def, *td_desc);
    auto ids = CollectArtifactIds(obj);
    EXPECT_EQ(ids.size(), 4);
    EXPECT_TRUE(ids.count(GenesisIds::kIndexDefinitionTypeDef));
    EXPECT_TRUE(ids.count(GenesisIds::kTypeDefinitionTypeDef));
    EXPECT_TRUE(ids.count(GenesisIds::kTypeVersionDefinitionTypeDef));
    EXPECT_TRUE(ids.count(GenesisIds::kReferenceDefinitionTypeDef));
  }

  // all_index_definitions: contains all 8 IndexDefinition artifact_ids
  {
    auto idx_def = FindIndexDef(*idx_desc, "all_index_definitions");
    std::vector<uint8_t> empty_key;
    auto obj = ReadIndexObject(storage_, GenesisIds::kAllIndexDefinitions, empty_key, idx_def, *idx_desc);
    auto ids = CollectArtifactIds(obj);
    EXPECT_EQ(ids.size(), 8);
    EXPECT_TRUE(ids.count(GenesisIds::kIndexKeyTypeUnique));
    EXPECT_TRUE(ids.count(GenesisIds::kAllIndexDefinitions));
    EXPECT_TRUE(ids.count(GenesisIds::kTypeNameUnique));
    EXPECT_TRUE(ids.count(GenesisIds::kAllTypes));
    EXPECT_TRUE(ids.count(GenesisIds::kTypeVersionsByType));
    EXPECT_TRUE(ids.count(GenesisIds::kReferenceKeyTypeUnique));
    EXPECT_TRUE(ids.count(GenesisIds::kReferencesByTargetType));
    EXPECT_TRUE(ids.count(GenesisIds::kAllReferenceDefinitions));
  }

  // index_key_type_unique: look up each key_type
  {
    auto idx_def = FindIndexDef(*idx_desc, "index_key_type_unique");
    struct Case {
      std::string key_type;
      uint64_t expected_id;
    };
    const Case cases[] = {
        {"index_key_type_unique", GenesisIds::kIndexKeyTypeUnique},
        {"all_index_definitions", GenesisIds::kAllIndexDefinitions},
        {"type_name_unique", GenesisIds::kTypeNameUnique},
        {"all_types", GenesisIds::kAllTypes},
        {"type_versions_by_type", GenesisIds::kTypeVersionsByType},
        {"reference_key_type_unique", GenesisIds::kReferenceKeyTypeUnique},
        {"references_by_target_type", GenesisIds::kReferencesByTargetType},
        {"all_reference_definitions", GenesisIds::kAllReferenceDefinitions},
    };
    for (const auto& c : cases) {
      SCOPED_TRACE(c.key_type);
      auto key = EncodeStringKey(*idx_desc, "key_type", c.key_type);
      auto obj = ReadIndexObject(storage_, GenesisIds::kIndexKeyTypeUnique, key, idx_def, *idx_desc);
      ASSERT_EQ(obj.rows.size(), 1);
      EXPECT_EQ(obj.rows[0].artifact_id, c.expected_id);
    }
  }

  // type_versions_by_type: look up each type_id
  {
    auto idx_def = FindIndexDef(*tvd_desc, "type_versions_by_type");
    struct Case {
      uint64_t type_id;
      uint64_t expected_tvd_id;
    };
    const Case cases[] = {
        {GenesisIds::kIndexDefinitionTypeDef, GenesisIds::kIndexDefinitionTypeVersionDef},
        {GenesisIds::kTypeDefinitionTypeDef, GenesisIds::kTypeDefinitionTypeVersionDef},
        {GenesisIds::kTypeVersionDefinitionTypeDef, GenesisIds::kTypeVersionDefinitionTypeVersionDef},
        {GenesisIds::kReferenceDefinitionTypeDef, GenesisIds::kReferenceDefinitionTypeVersionDef},
    };
    for (const auto& c : cases) {
      SCOPED_TRACE(c.type_id);
      auto key = EncodeUint64Key(*tvd_desc, "type_id", c.type_id);
      auto obj = ReadIndexObject(storage_, GenesisIds::kTypeVersionsByType, key, idx_def, *tvd_desc);
      ASSERT_EQ(obj.rows.size(), 1);
      EXPECT_EQ(obj.rows[0].artifact_id, c.expected_tvd_id);
    }
  }

  // reference_key_type_unique: the one reference key_type
  {
    auto idx_def = FindIndexDef(*rd_desc, "reference_key_type_unique");
    auto key = EncodeStringKey(*rd_desc, "key_type", "artifact_system.TypeVersionDefinition.type_id");
    auto obj = ReadIndexObject(storage_, GenesisIds::kReferenceKeyTypeUnique, key, idx_def, *rd_desc);
    ASSERT_EQ(obj.rows.size(), 1);
    EXPECT_EQ(obj.rows[0].artifact_id, GenesisIds::kRefTypeVersionDefTypeId);
  }

  // references_by_target_type: look up TypeDefinition
  {
    auto idx_def = FindIndexDef(*rd_desc, "references_by_target_type");
    auto key = EncodeStringKey(*rd_desc, "target_type_name", "artifact_system.TypeDefinition");
    auto obj = ReadIndexObject(storage_, GenesisIds::kReferencesByTargetType, key, idx_def, *rd_desc);
    auto ids = CollectArtifactIds(obj);
    EXPECT_TRUE(ids.count(GenesisIds::kRefTypeVersionDefTypeId));
  }

  // all_reference_definitions: contains the one ReferenceDefinition
  {
    auto idx_def = FindIndexDef(*rd_desc, "all_reference_definitions");
    std::vector<uint8_t> empty_key;
    auto obj = ReadIndexObject(storage_, GenesisIds::kAllReferenceDefinitions, empty_key, idx_def, *rd_desc);
    auto ids = CollectArtifactIds(obj);
    EXPECT_EQ(ids.size(), 1);
    EXPECT_TRUE(ids.count(GenesisIds::kRefTypeVersionDefTypeId));
  }
}

TEST_F(GenesisTest, PostGenesisRegisterTypeVersionWorks) {
  MockIdAllocator id_alloc(GenesisIds::kFirstUserAllocatableId);
  TransactionManager txn_mgr(&storage_);
  TypeRegistry registry(&storage_, &txn_mgr, &id_alloc, genesis_result_.index_def_ids_by_key_type);

  const std::string proto_source = R"(
syntax = "proto3";
package test;
import "artifact_options.proto";
message MyType {
  option (artifact_system.indexes) = {
    key_type: "my_type_by_name"
    key: ["name"]
    order: { field: "artifact_id" direction: ASCENDING }
    unique: true
  };
  string name = 1;
}
)";

  auto result = registry.RegisterTypeVersion("test.MyType", proto_source);
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_GT(result->version_id, 0);

  auto versions = registry.ListTypeVersions("test.MyType");
  ASSERT_TRUE(versions.ok()) << versions.status();
  EXPECT_EQ(versions->size(), 1);
}

} // namespace
} // namespace artifact_system::testing

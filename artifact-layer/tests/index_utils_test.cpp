#include "index/index_utils.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "artifact_options.pb.h"
#include "artifact_types.pb.h"
#include "encoding/index_key_encoder.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "storage/memory_storage.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

// Build an IndexDefinition with the given uniqueness.
IndexDefinition BuildIndexDefinition(bool unique) {
  IndexDefinition definition;
  definition.set_key_type("type_name_unique");
  definition.set_unique(unique);
  definition.add_key("type_name");
  auto* order = definition.add_order();
  order->set_field("artifact_id");
  order->set_direction(OrderDefinition::ASCENDING);
  return definition;
}

// Encode a string key for TypeDefinition's type_name field.
std::vector<uint8_t> EncodeTypeName(const std::string& name) {
  const auto* desc = TypeDefinition::descriptor();
  auto encoded_or = encoding::EncodeSingleStringKey(*desc, "type_name", name);
  EXPECT_TRUE(encoded_or.ok()) << encoded_or.status();
  return *encoded_or;
}

// Build a DerivedIndexEntry for a given key.
index::DerivedIndexEntry MakeEntry(uint64_t index_def_id, const std::string& type_name, uint64_t artifact_id) {
  index::DerivedIndexEntry entry;
  entry.index_def_id = index_def_id;
  entry.key_type = "type_name_unique";
  entry.encoded_key = EncodeTypeName(type_name);
  entry.order_values.push_back(static_cast<uint64_t>(artifact_id));
  entry.key_values = {type_name};
  return entry;
}

TEST(IndexUtilsTest, AddIndexRowRejectsUniqueViolationOnSequentialWrites) {
  MemoryStorage storage;
  const std::string branch = storage.GetCanonicalBranch();
  const auto* descriptor = TypeDefinition::descriptor();
  const IndexDefinition index_def = BuildIndexDefinition(/*unique=*/true);
  constexpr uint64_t kIndexDefId = 100;

  // First write succeeds.
  auto entry1 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/1);
  auto status1 = index::AddIndexRow(&storage, branch, entry1, 1, index_def, *descriptor);
  ASSERT_TRUE(status1.ok()) << status1;

  // Second write to the same key with a different artifact_id must fail.
  auto entry2 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/2);
  auto status2 = index::AddIndexRow(&storage, branch, entry2, 2, index_def, *descriptor);
  ASSERT_FALSE(status2.ok());
  EXPECT_EQ(status2.code(), absl::StatusCode::kAborted);
}

TEST(IndexUtilsTest, AddIndexRowAllowsDuplicateKeyOnNonUniqueIndex) {
  MemoryStorage storage;
  const std::string branch = storage.GetCanonicalBranch();
  const auto* descriptor = TypeDefinition::descriptor();
  const IndexDefinition index_def = BuildIndexDefinition(/*unique=*/false);
  constexpr uint64_t kIndexDefId = 100;

  auto entry1 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/1);
  auto status1 = index::AddIndexRow(&storage, branch, entry1, 1, index_def, *descriptor);
  ASSERT_TRUE(status1.ok()) << status1;

  auto entry2 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/2);
  auto status2 = index::AddIndexRow(&storage, branch, entry2, 2, index_def, *descriptor);
  ASSERT_TRUE(status2.ok()) << status2;
}

TEST(IndexUtilsTest, AddIndexRowSucceedsAfterRemoveOnUniqueIndex) {
  MemoryStorage storage;
  const std::string branch = storage.GetCanonicalBranch();
  const auto* descriptor = TypeDefinition::descriptor();
  const IndexDefinition index_def = BuildIndexDefinition(/*unique=*/true);
  constexpr uint64_t kIndexDefId = 100;

  // First write.
  auto entry1 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/1);
  auto status1 = index::AddIndexRow(&storage, branch, entry1, 1, index_def, *descriptor);
  ASSERT_TRUE(status1.ok()) << status1;

  // Remove the first entry.
  auto remove_status = index::RemoveIndexRow(&storage, branch, entry1, 1, index_def, *descriptor);
  ASSERT_TRUE(remove_status.ok()) << remove_status;

  // Second write to the same key should now succeed.
  auto entry2 = MakeEntry(kIndexDefId, "my.Type", /*artifact_id=*/2);
  auto status2 = index::AddIndexRow(&storage, branch, entry2, 2, index_def, *descriptor);
  ASSERT_TRUE(status2.ok()) << status2;
}

} // namespace
} // namespace artifact_system::testing

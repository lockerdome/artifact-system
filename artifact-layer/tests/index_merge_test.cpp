#include "index/index_merge.h"

#include <cstdint>

#include "absl/status/status.h"
#include "artifact_options.pb.h"
#include "gtest/gtest.h"

namespace artifact_system::testing {
namespace {

artifact_system::IndexDefinition BuildIndexDefinition(bool unique) {
  artifact_system::IndexDefinition definition;
  definition.set_key_type("test_index");
  definition.set_unique(unique);
  auto* order = definition.add_order();
  order->set_field("artifact_id");
  order->set_direction(artifact_system::OrderDefinition::ASCENDING);
  return definition;
}

index::IndexRow MakeRow(uint64_t artifact_id) {
  return index::IndexRow{.artifact_id = artifact_id, .order_values = {artifact_id}};
}

TEST(IndexMergeTest, MergeNoConflictKeepsRows) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {MakeRow(1)}};
  index::IndexObject ours = base;
  index::IndexObject theirs = base;

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(merged_or.ok()) << merged_or.status();
  ASSERT_EQ(merged_or->merged.rows.size(), 1U);
  EXPECT_EQ(merged_or->merged.rows[0].artifact_id, 1U);
}

TEST(IndexMergeTest, MergeAddAddNonUniqueProducesBothRows) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {}};
  index::IndexObject ours{.serialized_key = "k", .rows = {MakeRow(1)}};
  index::IndexObject theirs{.serialized_key = "k", .rows = {MakeRow(2)}};

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(merged_or.ok()) << merged_or.status();
  ASSERT_EQ(merged_or->merged.rows.size(), 2U);
  EXPECT_EQ(merged_or->merged.rows[0].artifact_id, 1U);
  EXPECT_EQ(merged_or->merged.rows[1].artifact_id, 2U);
}

TEST(IndexMergeTest, MergeAddRemoveKeepsAdd) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {MakeRow(1)}};
  index::IndexObject ours{.serialized_key = "k", .rows = {MakeRow(1), MakeRow(2)}};
  index::IndexObject theirs{.serialized_key = "k", .rows = {}};

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(merged_or.ok()) << merged_or.status();
  ASSERT_EQ(merged_or->merged.rows.size(), 1U);
  EXPECT_EQ(merged_or->merged.rows[0].artifact_id, 2U);
}

TEST(IndexMergeTest, MergeRemoveRemoveProducesEmptyRows) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {MakeRow(1)}};
  index::IndexObject ours{.serialized_key = "k", .rows = {}};
  index::IndexObject theirs{.serialized_key = "k", .rows = {}};

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(merged_or.ok()) << merged_or.status();
  EXPECT_TRUE(merged_or->merged.rows.empty());
}

TEST(IndexMergeTest, MergeIsIdempotentForSameInputs) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {MakeRow(1)}};
  index::IndexObject ours{.serialized_key = "k", .rows = {MakeRow(1), MakeRow(3)}};
  index::IndexObject theirs{.serialized_key = "k", .rows = {MakeRow(1), MakeRow(2)}};

  auto once_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(once_or.ok()) << once_or.status();

  auto twice_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(twice_or.ok()) << twice_or.status();

  ASSERT_EQ(once_or->merged.rows.size(), twice_or->merged.rows.size());
  for (size_t i = 0; i < once_or->merged.rows.size(); ++i) {
    EXPECT_EQ(once_or->merged.rows[i].artifact_id, twice_or->merged.rows[i].artifact_id);
    EXPECT_EQ(once_or->merged.rows[i].order_values, twice_or->merged.rows[i].order_values);
  }
}

TEST(IndexMergeTest, UniqueIndexConflictIsReported) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(true);
  index::IndexObject base{.serialized_key = "k", .rows = {}};
  index::IndexObject ours{.serialized_key = "k", .rows = {MakeRow(10)}};
  index::IndexObject theirs{.serialized_key = "k", .rows = {MakeRow(11)}};

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_FALSE(merged_or.ok());
  EXPECT_EQ(merged_or.status().code(), absl::StatusCode::kAborted);
}

TEST(IndexMergeTest, DeduplicatesConflictingConcurrentUpdatesForSameArtifactId) {
  artifact_system::IndexDefinition definition;
  definition.set_key_type("test_index");
  definition.set_unique(false);
  auto* order0 = definition.add_order();
  order0->set_field("artifact_id");
  order0->set_direction(artifact_system::OrderDefinition::ASCENDING);
  auto* order1 = definition.add_order();
  order1->set_field("score");
  order1->set_direction(artifact_system::OrderDefinition::ASCENDING);

  index::IndexObject base{.serialized_key = "k",
                          .rows = {index::IndexRow{.artifact_id = 7, .order_values = {static_cast<uint64_t>(7), static_cast<int32_t>(1)}}}};
  index::IndexObject ours{.serialized_key = "k",
                          .rows = {index::IndexRow{.artifact_id = 7, .order_values = {static_cast<uint64_t>(7), static_cast<int32_t>(2)}}}};
  index::IndexObject theirs{.serialized_key = "k",
                            .rows = {index::IndexRow{.artifact_id = 7, .order_values = {static_cast<uint64_t>(7), static_cast<int32_t>(3)}}}};

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_TRUE(merged_or.ok()) << merged_or.status();
  ASSERT_EQ(merged_or->merged.rows.size(), 1U);
  EXPECT_EQ(merged_or->merged.rows[0].artifact_id, 7U);
  EXPECT_EQ(std::get<int32_t>(merged_or->merged.rows[0].order_values[1]), 2);
}

TEST(IndexMergeTest, RejectsMismatchedArtifactIdOrderValue) {
  const artifact_system::IndexDefinition definition = BuildIndexDefinition(false);
  index::IndexObject base{.serialized_key = "k", .rows = {index::IndexRow{.artifact_id = 7, .order_values = {static_cast<uint64_t>(8)}}}};
  index::IndexObject ours = base;
  index::IndexObject theirs = base;

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_FALSE(merged_or.ok());
  EXPECT_EQ(merged_or.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(IndexMergeTest, RejectsDuplicateArtifactIdOrderFields) {
  artifact_system::IndexDefinition definition;
  definition.set_key_type("test_index");
  definition.set_unique(false);
  auto* order0 = definition.add_order();
  order0->set_field("artifact_id");
  order0->set_direction(artifact_system::OrderDefinition::ASCENDING);
  auto* order1 = definition.add_order();
  order1->set_field("artifact_id");
  order1->set_direction(artifact_system::OrderDefinition::ASCENDING);

  index::IndexObject base{.serialized_key = "k",
                          .rows = {index::IndexRow{.artifact_id = 7, .order_values = {static_cast<uint64_t>(7), static_cast<uint64_t>(7)}}}};
  index::IndexObject ours = base;
  index::IndexObject theirs = base;

  auto merged_or = index::MergeIndexObjects(definition, base, ours, theirs);
  ASSERT_FALSE(merged_or.ok());
  EXPECT_EQ(merged_or.status().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace artifact_system::testing

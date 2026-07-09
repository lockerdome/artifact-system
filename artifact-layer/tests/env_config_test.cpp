#include "service/env_config.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace artifact_system::service {
namespace {

using ::testing::HasSubstr;

const char* const kEnvVars[] = {
    "ARTIFACT_LAYER_LISTEN_ADDRESS",
    "ARTIFACT_LAYER_STORAGE_TYPE",
    "LAKEFS_ENDPOINT",
    "LAKEFS_ACCESS_KEY_ID",
    "LAKEFS_SECRET_ACCESS_KEY",
    "LAKEFS_REPOSITORY",
    "LAKEFS_CANONICAL_BRANCH",
    "ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS",
    "ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID",
    "ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK",
};

// Clears all config-related environment variables for the duration of a test
// and restores the previous values afterward.
class EnvConfigTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (const char* name : kEnvVars) {
      const char* value = std::getenv(name);
      saved_.emplace_back(name, value != nullptr ? std::optional<std::string>(value) : std::nullopt);
      ::unsetenv(name);
    }
  }

  void TearDown() override {
    for (const auto& [name, value] : saved_) {
      if (value.has_value()) {
        ::setenv(name.c_str(), value->c_str(), 1);
      } else {
        ::unsetenv(name.c_str());
      }
    }
  }

  void SetEnv(const char* name, const char* value) {
    ::setenv(name, value, 1);
  }

  // Sets the full set of variables for a valid LakeFS + ID allocator config.
  void SetValidProductionEnv() {
    SetEnv("ARTIFACT_LAYER_STORAGE_TYPE", "lakefs");
    SetEnv("LAKEFS_ENDPOINT", "http://lakefs.example:8000");
    SetEnv("LAKEFS_ACCESS_KEY_ID", "test-access-key");
    SetEnv("LAKEFS_SECRET_ACCESS_KEY", "test-secret-key");
    SetEnv("LAKEFS_REPOSITORY", "artifacts");
    SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS", "id-allocator.example:50051");
    SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID", "artifact-layer");
  }

private:
  std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

TEST_F(EnvConfigTest, DefaultsToMemoryStorageAndMockAllocator) {
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->listen_address, "0.0.0.0:50051");
  EXPECT_FALSE(config->lakefs.has_value());
  EXPECT_FALSE(config->id_allocator.has_value());
}

TEST_F(EnvConfigTest, ReadsListenAddress) {
  SetEnv("ARTIFACT_LAYER_LISTEN_ADDRESS", "127.0.0.1:0");
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->listen_address, "127.0.0.1:0");
}

TEST_F(EnvConfigTest, ExplicitMemoryStorageType) {
  SetEnv("ARTIFACT_LAYER_STORAGE_TYPE", "memory");
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_FALSE(config->lakefs.has_value());
}

TEST_F(EnvConfigTest, UnknownStorageTypeIsRejected) {
  SetEnv("ARTIFACT_LAYER_STORAGE_TYPE", "postgres");
  auto config = LoadServerConfigFromEnv();
  ASSERT_FALSE(config.ok());
  EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(config.status().message()), HasSubstr("postgres"));
}

TEST_F(EnvConfigTest, LakeFSConfigIsPopulated) {
  SetValidProductionEnv();
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  ASSERT_TRUE(config->lakefs.has_value());
  EXPECT_EQ(config->lakefs->endpoint, "http://lakefs.example:8000");
  EXPECT_EQ(config->lakefs->access_key_id, "test-access-key");
  EXPECT_EQ(config->lakefs->secret_access_key, "test-secret-key");
  EXPECT_EQ(config->lakefs->repository, "artifacts");
  EXPECT_EQ(config->lakefs->canonical_branch, "main");
}

TEST_F(EnvConfigTest, LakeFSCanonicalBranchOverride) {
  SetValidProductionEnv();
  SetEnv("LAKEFS_CANONICAL_BRANCH", "canonical");
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  ASSERT_TRUE(config->lakefs.has_value());
  EXPECT_EQ(config->lakefs->canonical_branch, "canonical");
}

TEST_F(EnvConfigTest, LakeFSMissingVariablesAreAllNamed) {
  SetEnv("ARTIFACT_LAYER_STORAGE_TYPE", "lakefs");
  SetEnv("LAKEFS_ENDPOINT", "http://lakefs.example:8000");
  auto config = LoadServerConfigFromEnv();
  ASSERT_FALSE(config.ok());
  EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
  const std::string message(config.status().message());
  EXPECT_THAT(message, HasSubstr("LAKEFS_ACCESS_KEY_ID"));
  EXPECT_THAT(message, HasSubstr("LAKEFS_SECRET_ACCESS_KEY"));
  EXPECT_THAT(message, HasSubstr("LAKEFS_REPOSITORY"));
  EXPECT_THAT(message, Not(HasSubstr("LAKEFS_ENDPOINT")));
}

TEST_F(EnvConfigTest, LakeFSWithoutIdAllocatorIsRejected) {
  SetValidProductionEnv();
  ::unsetenv("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS");
  ::unsetenv("ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID");
  auto config = LoadServerConfigFromEnv();
  ASSERT_FALSE(config.ok());
  EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(config.status().message()), HasSubstr("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS"));
}

TEST_F(EnvConfigTest, IdAllocatorConfigIsPopulated) {
  SetValidProductionEnv();
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  ASSERT_TRUE(config->id_allocator.has_value());
  EXPECT_EQ(config->id_allocator->service_address, "id-allocator.example:50051");
  EXPECT_EQ(config->id_allocator->partition_id, "artifact-layer");
  EXPECT_EQ(config->id_allocator->high_water_mark, 1000u);
}

TEST_F(EnvConfigTest, IdAllocatorWithMemoryStorageIsAllowed) {
  SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS", "id-allocator.example:50051");
  SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID", "artifact-layer");
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_FALSE(config->lakefs.has_value());
  ASSERT_TRUE(config->id_allocator.has_value());
}

TEST_F(EnvConfigTest, IdAllocatorAddressRequiresPartitionId) {
  SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS", "id-allocator.example:50051");
  auto config = LoadServerConfigFromEnv();
  ASSERT_FALSE(config.ok());
  EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(config.status().message()), HasSubstr("ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID"));
}

TEST_F(EnvConfigTest, IdAllocatorHighWaterMarkOverride) {
  SetValidProductionEnv();
  SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK", "5000");
  auto config = LoadServerConfigFromEnv();
  ASSERT_TRUE(config.ok()) << config.status();
  ASSERT_TRUE(config->id_allocator.has_value());
  EXPECT_EQ(config->id_allocator->high_water_mark, 5000u);
}

TEST_F(EnvConfigTest, IdAllocatorHighWaterMarkMustBePositiveInteger) {
  SetValidProductionEnv();
  for (const char* bad_value : {"0", "-5", "1e3", "many"}) {
    SetEnv("ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK", bad_value);
    auto config = LoadServerConfigFromEnv();
    ASSERT_FALSE(config.ok()) << "value: " << bad_value;
    EXPECT_EQ(config.status().code(), absl::StatusCode::kInvalidArgument);
  }
}

} // namespace
} // namespace artifact_system::service

/// @file lakefs_storage_test.cpp
/// @brief Conformance tests for LakeFSStorage against a real LakeFS instance.
///
/// This test requires a running LakeFS instance. Configure via environment
/// variables:
///
///   LAKEFS_ENDPOINT       — LakeFS API endpoint (e.g., "http://localhost:8000")
///   LAKEFS_ACCESS_KEY_ID  — LakeFS access key ID
///   LAKEFS_SECRET_KEY     — LakeFS secret access key
///   LAKEFS_REPOSITORY     — LakeFS repository name
///
/// If LAKEFS_ENDPOINT is not set, all tests are skipped (allows CI to pass
/// without a LakeFS instance). The test creates a fresh repository for each
/// test case to ensure isolation.
///
/// Mark as an integration test: CI skips by default, runs on demand.

#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "gtest/gtest.h"

#include "storage/lakefs_config.h"
#include "storage/lakefs_storage.h"
#include "storage_conformance_test.h"

namespace artifact_system::testing {

namespace {

using json = nlohmann::json;

struct RepoCleanupInfo {
  std::string endpoint;
  std::string access_key_id;
  std::string secret_key;
  std::string repo_name;
};

std::mutex g_cleanup_mutex;
std::vector<RepoCleanupInfo> g_repos_to_cleanup;

bool EnsureCurlGlobalInit() {
  static std::once_flag init_once;
  static bool initialized = false;
  std::call_once(init_once, []() { initialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); });
  return initialized;
}

/// Get an environment variable, returning empty string if not set.
std::string GetEnv(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

/// Check if LakeFS integration tests are enabled (LAKEFS_ENDPOINT is set).
bool LakeFSEnabled() {
  return !GetEnv("LAKEFS_ENDPOINT").empty();
}

/// CURL write callback for setup/teardown HTTP requests.
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

/// Generate a unique repository name for test isolation.
std::string GenerateRepoName() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<int> dist(0, 999999);
  return "test-repo-" + std::to_string(dist(gen));
}

/// Create a LakeFS repository for testing. Returns true on success.
bool CreateRepository(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_key, const std::string& repo_name) {
  if (!EnsureCurlGlobalInit()) {
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl)
    return false;

  json body;
  body["name"] = repo_name;
  body["storage_namespace"] = "local://" + repo_name;
  body["default_branch"] = "main";
  std::string body_str = body.dump();

  std::string url = endpoint + "/api/v1/repositories";
  std::string response_body;
  std::string userpwd = access_key_id + ":" + secret_key;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);
  long status_code = 0;
  if (res == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  return (res == CURLE_OK && status_code == 201);
}

/// Delete a LakeFS repository (cleanup after test).
void DeleteRepository(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_key, const std::string& repo_name) {
  if (!EnsureCurlGlobalInit()) {
    return;
  }

  CURL* curl = curl_easy_init();
  if (!curl)
    return;

  std::string url = endpoint + "/api/v1/repositories/" + repo_name;
  std::string response_body;
  std::string userpwd = access_key_id + ":" + secret_key;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

  curl_easy_perform(curl);
  curl_easy_cleanup(curl);
}

void RegisterRepositoryForCleanup(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_key, const std::string& repo_name) {
  std::lock_guard<std::mutex> lock(g_cleanup_mutex);
  g_repos_to_cleanup.push_back(RepoCleanupInfo{.endpoint = endpoint, .access_key_id = access_key_id, .secret_key = secret_key, .repo_name = repo_name});
}

class LakeFSRepositoryCleanupEnvironment final : public ::testing::Environment {
public:
  void TearDown() override {
    std::vector<RepoCleanupInfo> repos;
    {
      std::lock_guard<std::mutex> lock(g_cleanup_mutex);
      repos.swap(g_repos_to_cleanup);
    }

    for (const auto& repo : repos) {
      DeleteRepository(repo.endpoint, repo.access_key_id, repo.secret_key, repo.repo_name);
    }
  }
};

[[maybe_unused]] const bool kCleanupEnvironmentRegistered = []() {
  ::testing::AddGlobalTestEnvironment(new LakeFSRepositoryCleanupEnvironment());
  return true;
}();

} // namespace

/// Factory for LakeFSStorage conformance tests.
///
/// Creates a fresh LakeFS repository for each test and cleans it up after.
struct LakeFSStorageFactory {
  static std::unique_ptr<StorageInterface> Create() {
    if (!LakeFSEnabled()) {
      return nullptr;
    }

    std::string endpoint = GetEnv("LAKEFS_ENDPOINT");
    std::string access_key_id = GetEnv("LAKEFS_ACCESS_KEY_ID");
    std::string secret_key = GetEnv("LAKEFS_SECRET_KEY");

    // Create a unique repo for this test.
    std::string repo_name = GenerateRepoName();

    if (!CreateRepository(endpoint, access_key_id, secret_key, repo_name)) {
      return nullptr;
    }
    RegisterRepositoryForCleanup(endpoint, access_key_id, secret_key, repo_name);

    LakeFSConfig config;
    config.endpoint = endpoint;
    config.access_key_id = access_key_id;
    config.secret_access_key = secret_key;
    config.repository = repo_name;
    config.canonical_branch = "main";
    config.timeout_seconds = 30;

    return std::make_unique<LakeFSStorage>(std::move(config));
  }
};

// Only instantiate the conformance suite if LakeFS is available.
// When LAKEFS_ENDPOINT is not set, the factory returns nullptr and the
// test fixture's SetUp will fail gracefully.
INSTANTIATE_TYPED_TEST_SUITE_P(LakeFS, StorageConformanceTest, LakeFSStorageFactory);

} // namespace artifact_system::testing

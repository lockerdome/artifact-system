#include "storage/lakefs_storage.h"
#include "storage_conformance_test.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace artifact_system::testing {
namespace {

using json = nlohmann::json;

struct RepoCleanupInfo {
  std::string endpoint;
  std::string access_key_id;
  std::string secret_access_key;
  std::string repo_name;
};

std::mutex g_cleanup_mutex;
std::vector<RepoCleanupInfo> g_repos_to_cleanup;

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

bool EnsureCurlGlobalInit() {
  static std::once_flag init_once;
  static bool initialized = false;

  std::call_once(init_once, []() { initialized = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); });
  return initialized;
}

std::string GetEnvOrEmpty(const char* key) {
  const char* value = std::getenv(key);
  return value ? std::string(value) : std::string();
}

std::string GetEnvOrDefault(const char* key, const char* default_value) {
  const char* value = std::getenv(key);
  return value ? std::string(value) : std::string(default_value);
}

std::string TrimTrailingSlash(std::string value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string BuildStorageNamespace(const std::string& storage_namespace_prefix, const std::string& repo_name) {
  if (storage_namespace_prefix.empty()) {
    return repo_name;
  }

  if (storage_namespace_prefix.back() == '/' || EndsWith(storage_namespace_prefix, "://")) {
    return storage_namespace_prefix + repo_name;
  }

  return storage_namespace_prefix + "/" + repo_name;
}

bool DoRequest(const std::string& method, const std::string& url, const std::string& access_key_id, const std::string& secret_access_key,
               const std::string& request_body, long* status_code, std::string* response_body) {
  if (!EnsureCurlGlobalInit()) {
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const std::string userpwd = access_key_id + ":" + secret_access_key;
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
    headers = curl_slist_append(headers, "Content-Type: application/json");
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  const CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return true;
}

bool CreateRepository(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_access_key, const std::string& repo_name,
                      const std::string& storage_namespace, const std::string& canonical_branch) {
  json body;
  body["name"] = repo_name;
  body["storage_namespace"] = storage_namespace;
  body["default_branch"] = canonical_branch;

  long status_code = 0;
  std::string response_body;
  const std::string url = TrimTrailingSlash(endpoint) + "/api/v1/repositories";
  const bool ok = DoRequest("POST", url, access_key_id, secret_access_key, body.dump(), &status_code, &response_body);
  if (!ok) {
    return false;
  }

  return status_code == 201 || status_code == 409;
}

void DeleteRepository(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_access_key, const std::string& repo_name) {
  long status_code = 0;
  std::string response_body;
  const std::string url = TrimTrailingSlash(endpoint) + "/api/v1/repositories/" + repo_name;
  (void)DoRequest("DELETE", url, access_key_id, secret_access_key, "", &status_code, &response_body);
}

void RegisterRepositoryForCleanup(const std::string& endpoint, const std::string& access_key_id, const std::string& secret_access_key,
                                  const std::string& repo_name) {
  std::lock_guard<std::mutex> lock(g_cleanup_mutex);
  g_repos_to_cleanup.push_back(RepoCleanupInfo{endpoint, access_key_id, secret_access_key, repo_name});
}

std::string GenerateRepositoryName() {
  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  std::mt19937_64 rng(static_cast<std::mt19937_64::result_type>(now));
  std::uniform_int_distribution<unsigned long long> dist;

  std::ostringstream oss;
  oss << "artifact-layer-it-" << std::hex << dist(rng);
  return oss.str();
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
      DeleteRepository(repo.endpoint, repo.access_key_id, repo.secret_access_key, repo.repo_name);
    }
  }
};

[[maybe_unused]] const bool kCleanupEnvironmentRegistered = []() {
  ::testing::AddGlobalTestEnvironment(new LakeFSRepositoryCleanupEnvironment());
  return true;
}();

} // namespace

struct LakeFSStorageFactory {
  static std::unique_ptr<StorageInterface> Create() {
    const std::string endpoint = GetEnvOrEmpty("LAKEFS_ENDPOINT");
    const std::string access_key_id = GetEnvOrEmpty("LAKEFS_ACCESS_KEY_ID");
    const std::string secret_access_key = GetEnvOrEmpty("LAKEFS_SECRET_ACCESS_KEY");

    if (endpoint.empty() || access_key_id.empty() || secret_access_key.empty()) {
      return nullptr;
    }

    const std::string storage_namespace_prefix = GetEnvOrEmpty("LAKEFS_STORAGE_NAMESPACE_PREFIX");
    const std::string storage_bucket = GetEnvOrDefault("LAKEFS_STORAGE_BUCKET", "lakefs");
    const std::string canonical_branch = GetEnvOrDefault("LAKEFS_CANONICAL_BRANCH", "main");

    const std::string repo_name = GenerateRepositoryName();
    const std::string storage_namespace =
        storage_namespace_prefix.empty() ? "s3://" + storage_bucket + "/" + repo_name : BuildStorageNamespace(storage_namespace_prefix, repo_name);

    if (!CreateRepository(endpoint, access_key_id, secret_access_key, repo_name, storage_namespace, canonical_branch)) {
      return nullptr;
    }

    RegisterRepositoryForCleanup(endpoint, access_key_id, secret_access_key, repo_name);

    LakeFSConfig config;
    config.endpoint = endpoint;
    config.access_key_id = access_key_id;
    config.secret_access_key = secret_access_key;
    config.repository = repo_name;
    config.canonical_branch = canonical_branch;

    return std::make_unique<LakeFSStorage>(std::move(config));
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(LakeFS, StorageConformanceTest, LakeFSStorageFactory);

} // namespace artifact_system::testing

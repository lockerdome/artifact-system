#include "storage/lakefs_storage.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace artifact_system {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// curl write callback
// ---------------------------------------------------------------------------

namespace {

absl::Status EnsureCurlGlobalInit() {
  static std::once_flag init_once;
  static CURLcode init_result = CURLE_OK;
  std::call_once(init_once, []() { init_result = curl_global_init(CURL_GLOBAL_DEFAULT); });

  if (init_result != CURLE_OK) {
    return absl::InternalError(absl::StrCat("curl_global_init failed: ", curl_easy_strerror(init_result)));
  }
  return absl::OkStatus();
}

/// libcurl write callback — appends received data to a std::string.
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* response_body = static_cast<std::string*>(userdata);
  size_t total = size * nmemb;
  response_body->append(ptr, total);
  return total;
}

/// Map an HTTP status code to an absl::Status for general API errors.
/// Caller may override for specific endpoints (e.g., 409 on branch create).
absl::Status HttpStatusToAbsl(long code, const std::string& body, const std::string& context) {
  if (code >= 200 && code < 300) {
    return absl::InternalError(absl::StrCat(context, ": unexpected HTTP success status ", code, " — ", body));
  }
  if (code == 404) {
    return absl::NotFoundError(absl::StrCat(context, ": not found — ", body));
  }
  if (code == 409) {
    return absl::AlreadyExistsError(absl::StrCat(context, ": conflict — ", body));
  }
  if (code == 412) {
    return absl::FailedPreconditionError(absl::StrCat(context, ": precondition failed — ", body));
  }
  if (code == 403) {
    return absl::PermissionDeniedError(absl::StrCat(context, ": forbidden — ", body));
  }
  if (code == 410) {
    return absl::NotFoundError(absl::StrCat(context, ": gone — ", body));
  }
  if (code == 400) {
    return absl::InvalidArgumentError(absl::StrCat(context, ": bad request — ", body));
  }
  if (code >= 500) {
    return absl::InternalError(absl::StrCat(context, ": server error (", code, ") — ", body));
  }
  return absl::UnknownError(absl::StrCat(context, ": HTTP ", code, " — ", body));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / Destruction / Move
// ---------------------------------------------------------------------------

LakeFSStorage::LakeFSStorage(LakeFSConfig config) : config_(std::move(config)) {
  (void)EnsureCurlGlobalInit();
}

LakeFSStorage::~LakeFSStorage() = default;

// ---------------------------------------------------------------------------
// Internal HTTP helpers
// ---------------------------------------------------------------------------

std::string LakeFSStorage::ApiUrl(const std::string& api_path) const {
  return absl::StrCat(config_.endpoint, "/api/v1", api_path);
}

std::string LakeFSStorage::UrlEncode(const std::string& value) {
  // Use a temporary curl handle for encoding so we don't disturb the main
  // handle's state.  curl_easy_escape requires a valid handle but does not
  // perform I/O.
  CURL* tmp = curl_easy_init();
  if (!tmp) {
    // Fallback: return the value unencoded (should never happen).
    return value;
  }
  char* encoded = curl_easy_escape(tmp, value.c_str(), static_cast<int>(value.size()));
  std::string result(encoded ? encoded : value);
  if (encoded) {
    curl_free(encoded);
  }
  curl_easy_cleanup(tmp);
  return result;
}

LakeFSStorage::HttpResponse LakeFSStorage::DoRequest(const std::string& method, const std::string& api_path, const std::string& body,
                                                     const std::string& content_type, const std::string& accept_type) {
  HttpResponse response;

  auto init_status = EnsureCurlGlobalInit();
  if (!init_status.ok()) {
    response.status_code = 0;
    response.body = std::string(init_status.message());
    return response;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.status_code = 0;
    response.body = "failed to initialize curl handle";
    return response;
  }

  std::string url = ApiUrl(api_path);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Basic auth.
  std::string userpwd = absl::StrCat(config_.access_key_id, ":", config_.secret_access_key);
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

  // Timeout.
  if (config_.timeout_seconds > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
  }

  // Method + body.
  struct curl_slist* headers = nullptr;

  if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    std::string ct_header = absl::StrCat("Content-Type: ", content_type);
    headers = curl_slist_append(headers, ct_header.c_str());
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else if (method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    std::string ct_header = absl::StrCat("Content-Type: ", content_type);
    headers = curl_slist_append(headers, ct_header.c_str());
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  std::string accept_header = absl::StrCat("Accept: ", accept_type);
  headers = curl_slist_append(headers, accept_header.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    response.status_code = 0;
    response.body = absl::StrCat("curl error: ", curl_easy_strerror(res));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

LakeFSStorage::HttpResponse LakeFSStorage::DoUpload(const std::string& api_path, const std::string& data) {
  HttpResponse response;

  auto init_status = EnsureCurlGlobalInit();
  if (!init_status.ok()) {
    response.status_code = 0;
    response.body = std::string(init_status.message());
    return response;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.status_code = 0;
    response.body = "failed to initialize curl handle";
    return response;
  }

  std::string url = ApiUrl(api_path);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Basic auth.
  std::string userpwd = absl::StrCat(config_.access_key_id, ":", config_.secret_access_key);
  curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

  // Timeout.
  if (config_.timeout_seconds > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
  }

  // Build multipart form with "content" field.
  curl_mime* mime = curl_mime_init(curl);
  if (!mime) {
    response.status_code = 0;
    response.body = "failed to initialize multipart form";
    curl_easy_cleanup(curl);
    return response;
  }

  curl_mimepart* part = curl_mime_addpart(mime);
  if (!part) {
    response.status_code = 0;
    response.body = "failed to create multipart form part";
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return response;
  }
  curl_mime_name(part, "content");
  curl_mime_data(part, data.c_str(), data.size());
  curl_mime_filename(part, "content");
  curl_mime_type(part, "application/octet-stream");

  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

  // Accept JSON response.
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  CURLcode res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    response.status_code = 0;
    response.body = absl::StrCat("curl error: ", curl_easy_strerror(res));
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
  }

  curl_mime_free(mime);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

absl::StatusOr<bool> LakeFSStorage::BranchExists(const std::string& branch) {
  auto encoded_branch = UrlEncode(branch);
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", encoded_branch);
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("BranchExists: ", resp.body));
  }
  if (resp.status_code == 200) {
    return true;
  }
  if (resp.status_code == 404) {
    return false;
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "BranchExists");
}

absl::StatusOr<bool> LakeFSStorage::CommitExists(const std::string& commit_id) {
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/commits/", UrlEncode(commit_id));
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("CommitExists: ", resp.body));
  }
  if (resp.status_code == 200) {
    return true;
  }
  if (resp.status_code == 404) {
    return false;
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "CommitExists");
}

absl::StatusOr<bool> LakeFSStorage::ResolveRefKind(const std::string& ref) {
  auto branch_exists = BranchExists(ref);
  if (!branch_exists.ok()) {
    return branch_exists.status();
  }
  if (*branch_exists) {
    return true;
  }

  auto commit_exists = CommitExists(ref);
  if (!commit_exists.ok()) {
    return commit_exists.status();
  }
  if (*commit_exists) {
    return false;
  }

  return absl::NotFoundError(absl::StrCat("ref not found: ", ref));
}

absl::StatusOr<bool> LakeFSStorage::HasUncommittedChanges(const std::string& branch) {
  auto encoded_branch = UrlEncode(branch);
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", encoded_branch, "/diff?amount=1");
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("HasUncommittedChanges: ", resp.body));
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  if (resp.status_code != 200) {
    return HttpStatusToAbsl(resp.status_code, resp.body, "HasUncommittedChanges");
  }

  auto parsed = json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    return absl::InternalError("HasUncommittedChanges: failed to parse JSON response");
  }

  auto results_it = parsed.find("results");
  if (results_it == parsed.end() || !results_it->is_array()) {
    return false;
  }
  return !results_it->empty();
}

absl::StatusOr<std::string> LakeFSStorage::FindMergeBase(const std::string& source_ref, const std::string& dest_ref) {
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(source_ref), "/merge/", UrlEncode(dest_ref));
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("FindMergeBase: ", resp.body));
  }
  if (resp.status_code != 200) {
    return HttpStatusToAbsl(resp.status_code, resp.body, "FindMergeBase");
  }

  auto parsed = json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    return absl::InternalError("FindMergeBase: failed to parse JSON response");
  }

  auto base_it = parsed.find("base_commit_id");
  if (base_it == parsed.end() || !base_it->is_string()) {
    return absl::InternalError("FindMergeBase: missing base_commit_id in response");
  }
  return base_it->get<std::string>();
}

absl::StatusOr<std::vector<std::string>> LakeFSStorage::GetConflictingPaths(const std::string& source_ref, const std::string& dest_ref) {
  std::vector<std::string> conflicts;
  std::string after;

  // Paginate through the three-dot diff to collect all conflict entries.
  while (true) {
    auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(source_ref), "/diff/", UrlEncode(dest_ref),
                                 "?amount=1000&type=three_dot");
    if (!after.empty()) {
      absl::StrAppend(&api_path, "&after=", UrlEncode(after));
    }

    auto resp = DoRequest("GET", api_path);

    if (resp.status_code == 0) {
      return absl::UnavailableError(absl::StrCat("GetConflictingPaths: ", resp.body));
    }
    if (resp.status_code != 200) {
      return HttpStatusToAbsl(resp.status_code, resp.body, "GetConflictingPaths");
    }

    auto parsed = json::parse(resp.body, nullptr, false);
    if (parsed.is_discarded()) {
      return absl::InternalError("GetConflictingPaths: failed to parse JSON response");
    }

    auto results_it = parsed.find("results");
    if (results_it != parsed.end() && results_it->is_array()) {
      for (const auto& entry : *results_it) {
        auto type_it = entry.find("type");
        if (type_it != entry.end() && type_it->is_string() && type_it->get<std::string>() == "conflict") {
          auto path_it = entry.find("path");
          if (path_it != entry.end() && path_it->is_string()) {
            conflicts.push_back(path_it->get<std::string>());
          }
        }
      }
    }

    // Check pagination.
    auto pagination_it = parsed.find("pagination");
    if (pagination_it == parsed.end()) {
      break;
    }
    auto has_more_it = pagination_it->find("has_more");
    if (has_more_it == pagination_it->end() || !has_more_it->is_boolean() || !has_more_it->get<bool>()) {
      break;
    }
    auto next_offset_it = pagination_it->find("next_offset");
    if (next_offset_it == pagination_it->end() || !next_offset_it->is_string()) {
      break;
    }
    after = next_offset_it->get<std::string>();
  }

  std::sort(conflicts.begin(), conflicts.end());
  return conflicts;
}

absl::StatusOr<LakeFSStorage::HttpResponse> LakeFSStorage::StatObject(const std::string& ref, const std::string& path) {
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(ref), "/objects/stat?path=", UrlEncode(path));
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("StatObject: ", resp.body));
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("object not found: ", path, " at ref ", ref));
  }
  if (resp.status_code != 200) {
    return HttpStatusToAbsl(resp.status_code, resp.body, "StatObject");
  }
  return resp;
}

// ---------------------------------------------------------------------------
// Branch operations
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> LakeFSStorage::CreateBranch(const std::string& name, const std::string& base_commit_id) {
  // Preserve duplicate branch semantics required by StorageInterface.
  auto name_is_branch = BranchExists(name);
  if (!name_is_branch.ok()) {
    return name_is_branch.status();
  }
  if (*name_is_branch) {
    return absl::AlreadyExistsError(absl::StrCat("branch already exists: ", name));
  }

  // Guard against ambiguous refs: branch names must not collide with commit IDs.
  auto name_is_commit = CommitExists(name);
  if (!name_is_commit.ok()) {
    return name_is_commit.status();
  }
  if (*name_is_commit) {
    return absl::InvalidArgumentError(absl::StrCat("branch name collides with commit ID: ", name));
  }

  std::string source = base_commit_id;

  if (source.empty()) {
    // Fork from the canonical branch head.
    source = config_.canonical_branch;
  } else {
    auto source_is_commit = CommitExists(source);
    if (!source_is_commit.ok()) {
      return source_is_commit.status();
    }
    if (!*source_is_commit) {
      return absl::NotFoundError(absl::StrCat("base commit not found: ", source));
    }
  }

  json body;
  body["name"] = name;
  body["source"] = source;

  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches");
  auto resp = DoRequest("POST", api_path, body.dump());

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("CreateBranch: ", resp.body));
  }
  if (resp.status_code == 201) {
    return name;
  }
  if (resp.status_code == 409) {
    return absl::AlreadyExistsError(absl::StrCat("branch already exists: ", name));
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "CreateBranch");
}

absl::Status LakeFSStorage::DeleteBranch(const std::string& branch) {
  auto encoded_branch = UrlEncode(branch);
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", encoded_branch);
  auto resp = DoRequest("DELETE", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("DeleteBranch: ", resp.body));
  }
  if (resp.status_code == 204) {
    return absl::OkStatus();
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  if (resp.status_code == 403) {
    return absl::FailedPreconditionError("cannot delete canonical branch");
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "DeleteBranch");
}

absl::StatusOr<std::string> LakeFSStorage::GetBranchHead(const std::string& branch) {
  auto encoded_branch = UrlEncode(branch);
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", encoded_branch);
  auto resp = DoRequest("GET", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("GetBranchHead: ", resp.body));
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  if (resp.status_code != 200) {
    return HttpStatusToAbsl(resp.status_code, resp.body, "GetBranchHead");
  }

  auto parsed = json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    return absl::InternalError("GetBranchHead: failed to parse JSON response");
  }

  auto commit_id_it = parsed.find("commit_id");
  if (commit_id_it == parsed.end() || !commit_id_it->is_string()) {
    return absl::InternalError("GetBranchHead: missing commit_id in response");
  }
  return commit_id_it->get<std::string>();
}

// ---------------------------------------------------------------------------
// Object I/O
// ---------------------------------------------------------------------------

absl::Status LakeFSStorage::PutObject(const std::string& branch, const std::string& path, const std::string& data) {
  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", UrlEncode(branch), "/objects?path=", UrlEncode(path));
  auto resp = DoUpload(api_path, data);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("PutObject: ", resp.body));
  }
  if (resp.status_code == 201) {
    return absl::OkStatus();
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "PutObject");
}

absl::StatusOr<std::string> LakeFSStorage::GetObject(const std::string& ref, const std::string& path) {
  auto ref_kind = ResolveRefKind(ref);
  if (!ref_kind.ok()) {
    return ref_kind.status();
  }

  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(ref), "/objects?path=", UrlEncode(path));
  auto resp = DoRequest("GET", api_path, "", "application/json", "application/octet-stream");

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("GetObject: ", resp.body));
  }
  if (resp.status_code == 200) {
    return resp.body;
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("object not found: ", path, " at ref ", ref));
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "GetObject");
}

absl::Status LakeFSStorage::DeleteObject(const std::string& branch, const std::string& path) {
  // LakeFS DELETE object returns 204 on success. If the path doesn't exist but
  // the branch does, LakeFS may return 404. We need to distinguish "branch not
  // found" from "path not found on existing branch". Check branch existence
  // first.
  auto branch_exists = BranchExists(branch);
  if (!branch_exists.ok()) {
    return branch_exists.status();
  }
  if (!*branch_exists) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }

  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", UrlEncode(branch), "/objects?path=", UrlEncode(path));
  auto resp = DoRequest("DELETE", api_path);

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("DeleteObject: ", resp.body));
  }

  // 204 = deleted (tombstone staged). 404 = path didn't exist, which is fine
  // (LakeFS stages a tombstone anyway, or the path simply wasn't there).
  if (resp.status_code == 204 || resp.status_code == 404) {
    return absl::OkStatus();
  }
  return HttpStatusToAbsl(resp.status_code, resp.body, "DeleteObject");
}

absl::StatusOr<bool> LakeFSStorage::ObjectExists(const std::string& ref, const std::string& path) {
  auto ref_kind = ResolveRefKind(ref);
  if (!ref_kind.ok()) {
    return ref_kind.status();
  }

  auto result = StatObject(ref, path);
  if (result.ok()) {
    return true;
  }
  if (result.status().code() == absl::StatusCode::kNotFound) {
    return false;
  }
  return result.status();
}
absl::StatusOr<std::vector<std::string>> LakeFSStorage::ListObjects(const std::string& ref, const std::string& prefix) {
  auto ref_kind = ResolveRefKind(ref);
  if (!ref_kind.ok()) {
    return ref_kind.status();
  }

  std::vector<std::string> paths;
  std::string after;

  while (true) {
    auto api_path =
        absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(ref), "/objects/ls?prefix=", UrlEncode(prefix), "&amount=1000");
    if (!after.empty()) {
      absl::StrAppend(&api_path, "&after=", UrlEncode(after));
    }

    auto resp = DoRequest("GET", api_path);

    if (resp.status_code == 0) {
      return absl::UnavailableError(absl::StrCat("ListObjects: ", resp.body));
    }
    if (resp.status_code == 404) {
      return absl::NotFoundError(absl::StrCat("ref not found: ", ref));
    }
    if (resp.status_code != 200) {
      return HttpStatusToAbsl(resp.status_code, resp.body, "ListObjects");
    }

    auto parsed = json::parse(resp.body, nullptr, false);
    if (parsed.is_discarded()) {
      return absl::InternalError("ListObjects: failed to parse JSON response");
    }

    auto results_it = parsed.find("results");
    if (results_it != parsed.end() && results_it->is_array()) {
      for (const auto& entry : *results_it) {
        // Only include actual objects, not directory markers.
        auto path_type_it = entry.find("path_type");
        if (path_type_it != entry.end() && path_type_it->is_string() && path_type_it->get<std::string>() != "object") {
          continue;
        }
        auto path_it = entry.find("path");
        if (path_it != entry.end() && path_it->is_string()) {
          paths.push_back(path_it->get<std::string>());
        }
      }
    }

    // Check pagination.
    auto pagination_it = parsed.find("pagination");
    if (pagination_it == parsed.end()) {
      break;
    }
    auto has_more_it = pagination_it->find("has_more");
    if (has_more_it == pagination_it->end() || !has_more_it->is_boolean() || !has_more_it->get<bool>()) {
      break;
    }
    auto next_offset_it = pagination_it->find("next_offset");
    if (next_offset_it == pagination_it->end() || !next_offset_it->is_string()) {
      break;
    }
    after = next_offset_it->get<std::string>();
  }

  std::sort(paths.begin(), paths.end());
  return paths;
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

absl::StatusOr<std::string> LakeFSStorage::Commit(const std::string& branch, const std::string& message) {
  json body;
  body["message"] = message;
  body["allow_empty"] = true;

  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/branches/", UrlEncode(branch), "/commits");
  auto resp = DoRequest("POST", api_path, body.dump());

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("Commit: ", resp.body));
  }
  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("branch not found: ", branch));
  }
  if (resp.status_code != 201) {
    return HttpStatusToAbsl(resp.status_code, resp.body, "Commit");
  }

  auto parsed = json::parse(resp.body, nullptr, false);
  if (parsed.is_discarded()) {
    return absl::InternalError("Commit: failed to parse JSON response");
  }

  auto id_it = parsed.find("id");
  if (id_it == parsed.end() || !id_it->is_string()) {
    return absl::InternalError("Commit: missing id in response");
  }
  return id_it->get<std::string>();
}

// ---------------------------------------------------------------------------
// Merge
// ---------------------------------------------------------------------------

absl::StatusOr<MergeResult> LakeFSStorage::Merge(const std::string& source, const std::string& target) {
  if (source == target) {
    return absl::InvalidArgumentError(absl::StrCat("source and target branches must differ: ", source));
  }

  // Pre-check: both branches must have no uncommitted changes.
  auto source_uncommitted = HasUncommittedChanges(source);
  if (!source_uncommitted.ok()) {
    return source_uncommitted.status();
  }
  if (*source_uncommitted) {
    return absl::FailedPreconditionError(absl::StrCat("source branch '", source, "' has uncommitted changes"));
  }

  auto target_uncommitted = HasUncommittedChanges(target);
  if (!target_uncommitted.ok()) {
    return target_uncommitted.status();
  }
  if (*target_uncommitted) {
    return absl::FailedPreconditionError(absl::StrCat("target branch '", target, "' has uncommitted changes"));
  }

  // Perform the merge.
  json body;
  body["message"] = absl::StrCat("Merge '", source, "' into '", target, "'");
  body["allow_empty"] = true;

  auto api_path = absl::StrCat("/repositories/", UrlEncode(config_.repository), "/refs/", UrlEncode(source), "/merge/", UrlEncode(target));
  auto resp = DoRequest("POST", api_path, body.dump());

  if (resp.status_code == 0) {
    return absl::UnavailableError(absl::StrCat("Merge: ", resp.body));
  }

  // Success: merge commit created.
  if (resp.status_code == 200) {
    auto parsed = json::parse(resp.body, nullptr, false);
    if (parsed.is_discarded()) {
      return absl::InternalError("Merge: failed to parse success JSON response");
    }

    auto ref_it = parsed.find("reference");
    if (ref_it == parsed.end() || !ref_it->is_string()) {
      return absl::InternalError("Merge: missing reference in response");
    }

    MergeResult result;
    result.result = MergeResult::Success{.commit_id = ref_it->get<std::string>()};
    return result;
  }

  // Conflict: need to populate MergeResult::Conflict.
  if (resp.status_code == 409) {
    // Get the merge base.
    auto merge_base = FindMergeBase(source, target);
    if (!merge_base.ok()) {
      return merge_base.status();
    }

    // Get the conflicting paths via three-dot diff.
    auto conflicting = GetConflictingPaths(source, target);
    if (!conflicting.ok()) {
      return conflicting.status();
    }

    // Get the head commit IDs for source and target.
    auto source_head = GetBranchHead(source);
    if (!source_head.ok()) {
      return source_head.status();
    }
    auto target_head = GetBranchHead(target);
    if (!target_head.ok()) {
      return target_head.status();
    }

    MergeResult result;
    result.result = MergeResult::Conflict{
        .conflicting_paths = std::move(*conflicting),
        .base_commit_id = std::move(*merge_base),
        .source_commit_id = std::move(*source_head),
        .target_commit_id = std::move(*target_head),
    };
    return result;
  }

  // 412: Precondition failed (uncommitted changes detected server-side).
  if (resp.status_code == 412) {
    return absl::FailedPreconditionError(absl::StrCat("Merge: uncommitted changes — ", resp.body));
  }

  // LakeFS may also report merge precondition failures as 400.
  if (resp.status_code == 400) {
    return absl::FailedPreconditionError(absl::StrCat("Merge: precondition failed — ", resp.body));
  }

  if (resp.status_code == 404) {
    return absl::NotFoundError(absl::StrCat("Merge: branch not found — ", resp.body));
  }

  if (resp.status_code >= 200 && resp.status_code < 300) {
    return absl::InternalError(absl::StrCat("Merge: unexpected HTTP success status ", resp.status_code));
  }

  return HttpStatusToAbsl(resp.status_code, resp.body, "Merge");
}

// ---------------------------------------------------------------------------
// Canonical branch
// ---------------------------------------------------------------------------

std::string LakeFSStorage::GetCanonicalBranch() const {
  return config_.canonical_branch;
}

} // namespace artifact_system

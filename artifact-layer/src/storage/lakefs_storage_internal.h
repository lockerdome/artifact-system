#pragma once

#include <string>

#include "absl/status/status.h"

namespace artifact_system::internal {

absl::Status EnsureCurlGlobalInit();

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

absl::Status MapDeleteBranchForbidden(const std::string& response_body);

} // namespace artifact_system::internal

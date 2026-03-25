#pragma once

#include <string>

#include "absl/status/status.h"

namespace artifact_system::internal {

absl::Status MapDeleteBranchForbidden(const std::string& response_body);

} // namespace artifact_system::internal

#pragma once

#include <string>

#include "absl/status/statusor.h"
#include "storage/storage_interface.h"
#include "transaction/conflict_resolver.h"

namespace artifact_system::index {

transaction::PathConflictKind IndexPathConflictClassifier(const std::string& path);

transaction::RetryConflictResolver BuildDeterministicIndexRetryConflictResolver(StorageInterface* storage);

} // namespace artifact_system::index

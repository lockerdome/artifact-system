#include "storage/memory_storage.h"
#include "storage_conformance_test.h"

namespace artifact_system::testing {

// Factory for MemoryStorage.
struct MemoryStorageFactory {
  static std::unique_ptr<StorageInterface> Create() {
    return std::make_unique<MemoryStorage>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(MemoryStorage, StorageConformanceTest, MemoryStorageFactory);

} // namespace artifact_system::testing

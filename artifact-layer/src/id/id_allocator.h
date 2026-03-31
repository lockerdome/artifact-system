#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "id/id_allocator_interface.h"

namespace id_allocator::client {
class IdAllocatorClient;
} // namespace id_allocator::client

namespace artifact_system {

struct IdAllocatorConfig {
  std::string service_address;
  std::string partition_id;
  uint64_t high_water_mark = 1000;
  uint32_t retry_max_retries = 5;
  uint32_t retry_base_delay_ms = 100;
  uint32_t retry_max_delay_ms = 10000;
};

/// Production implementation of IdAllocatorInterface wrapping
/// id_allocator::client::IdAllocatorClient.
///
/// Uses double-buffer pre-allocation: once the front buffer crosses the
/// high-water mark, the back buffer is filled asynchronously. When both
/// buffers are exhausted (prolonged ID service outage), AllocateId() throws
/// std::runtime_error. The gRPC service layer translates this to UNAVAILABLE.
class ProductionIdAllocator final : public IdAllocatorInterface {
public:
  explicit ProductionIdAllocator(IdAllocatorConfig config);
  ~ProductionIdAllocator() override;

  ProductionIdAllocator(const ProductionIdAllocator&) = delete;
  ProductionIdAllocator& operator=(const ProductionIdAllocator&) = delete;

  /// Initialize the underlying client. Must be called before AllocateId().
  void Initialize();

  [[nodiscard]] uint64_t AllocateId() override;

private:
  std::unique_ptr<id_allocator::client::IdAllocatorClient> client_;
};

} // namespace artifact_system

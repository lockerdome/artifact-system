#include "id/id_allocator.h"

#include "id_allocator_client/client.h"

namespace artifact_system {

ProductionIdAllocator::ProductionIdAllocator(IdAllocatorConfig config)
    : client_(std::make_unique<id_allocator::client::IdAllocatorClient>(id_allocator::client::IdAllocatorClientOptions{
          .service_address = std::move(config.service_address),
          .partition_id = std::move(config.partition_id),
          .high_water_mark = config.high_water_mark,
          .retry =
              {
                  .max_retries = config.retry_max_retries,
                  .base_delay_ms = config.retry_base_delay_ms,
                  .max_delay_ms = config.retry_max_delay_ms,
              },
      })) {
}

// The default destructor destroys client_, whose members are destroyed in
// reverse declaration order: buffer_ (joins prefetch thread) then grpc_client_.
// This ordering is safe because the prefetch thread finishes before the gRPC
// stub is destroyed. Do NOT call client_->close() here — its ordering
// (grpc_client_.close() before buffer_.close()) is unsafe when a prefetch is
// in flight.
ProductionIdAllocator::~ProductionIdAllocator() = default;

void ProductionIdAllocator::Initialize() {
  client_->initialize();
}

uint64_t ProductionIdAllocator::AllocateId() {
  return client_->allocate_id();
}

} // namespace artifact_system

#pragma once

#include <cstdint>
#include <stdexcept>

namespace artifact_system {

/// Abstract interface for allocating unique artifact IDs.
///
/// The production implementation wraps id_allocator::client::IdAllocatorClient
/// Tests use MockIdAllocator.
///
/// Throws std::runtime_error on failure, matching the real client's behavior.
/// The PRD specifies gRPC UNAVAILABLE for ID service exhaustion; the gRPC
/// service layer translates exceptions to gRPC status codes.
class IdAllocatorInterface {
public:
  virtual ~IdAllocatorInterface() = default;

  /// Allocate a single unique uint64 ID.
  /// @throws std::runtime_error if allocation fails (e.g., ID pool depleted).
  [[nodiscard]] virtual uint64_t AllocateId() = 0;
};

/// Test-only ID allocator that returns sequential IDs starting from a
/// configurable base. Thread-safe for single-threaded test use only.
class MockIdAllocator final : public IdAllocatorInterface {
public:
  explicit MockIdAllocator(uint64_t start_id = 1) : next_id_(start_id) {
  }

  [[nodiscard]] uint64_t AllocateId() override {
    return next_id_++;
  }

private:
  uint64_t next_id_;
};

} // namespace artifact_system

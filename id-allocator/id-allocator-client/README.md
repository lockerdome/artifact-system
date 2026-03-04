# id-allocator-client architecture

This package is a Node.js client for the ID Allocator gRPC service. It exposes a simple API (`initialize()`, `allocate_id()`, `close()`) while hiding network latency behind a local double-buffered ID cache.

## Goals

- Keep `allocate_id()` fast and synchronous in the common path.
- Reduce allocation latency spikes by prefetching the next ID block.
- Isolate transport concerns (gRPC, retries) from local allocation logic.

## High-level design

```
Application code
      |
      v
IdAllocatorClient (public API)
      |
      +--> BlockDoubleBuffer (front/back local ID blocks)
      |
      +--> IdAllocatorGrpcClient (gRPC transport + retries)
                |
                v
          ID Allocator service
```

## Main components

- `lib/client.js` (`IdAllocatorClient`)
  - Public facade used by application code.
  - Validates required options (`service_address`, `partition_id`).
  - Wires together the buffer layer and gRPC layer.

- `lib/block_double_buffer.js` (`BlockDoubleBuffer`)
  - Maintains two in-memory blocks:
    - **front**: actively serving IDs.
    - **back**: prefetched next block.
  - Triggers background prefetch when `front.remaining() <= high_water_mark`.
  - Swaps front/back when front is exhausted.

- `lib/block.js` (`Block`)
  - Small value object for contiguous ID ranges `[range_start, range_end)`.
  - Allocates IDs sequentially and tracks remaining capacity.

- `lib/grpc_client.js` (`IdAllocatorGrpcClient`)
  - Loads `../../proto/id_allocator.proto`.
  - Creates and owns the gRPC client/channel.
  - Fetches blocks via `AllocateBlock` RPC.
  - Applies retry policy for transient RPC failures.

- `lib/retry.js` (`retry_with_backoff`)
  - Exponential backoff with full jitter.
  - Configurable `max_retries`, `base_delay_ms`, and `max_delay_ms`.

## Allocation flow

1. App creates `IdAllocatorClient`.
2. App calls `await initialize()`.
   - gRPC client connects.
   - Initial block is fetched into the front buffer.
3. App calls `allocate_id()` repeatedly.
   - IDs are returned from front synchronously.
   - Near depletion, back buffer prefetch starts in background.
   - On depletion, front/back swap keeps allocations moving.
4. App calls `close()` to close the gRPC channel.

## Failure behavior

- Initialization failures are surfaced (connect/fetch errors throw).
- Background prefetch failures are logged and retried on later allocations when still below the high-water mark.
- If both buffers are empty, `allocate_id()` throws `ID pool depleted...`.
- RPC fetches use bounded retries before failing.

## Directory map

- `lib/index.js`: package export surface.
- `lib/client.js`: public client facade.
- `lib/block_double_buffer.js`: local buffering and prefetch logic.
- `lib/block.js`: single block data structure.
- `lib/grpc_client.js`: transport + proto wiring.
- `lib/retry.js`: retry utility.
- `tests/`: unit/integration tests (includes mock allocator server).

## Notes for maintainers

- `allocate_id()` is intentionally synchronous after initialization; avoid introducing await points in this hot path.
- Keep buffer logic and transport logic decoupled so each can be tested independently.
- Tune `high_water_mark` based on service latency and block size to balance memory use vs. depletion risk.

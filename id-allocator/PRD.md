# ID Allocator Service — Product Requirements Document

## Overview

The ID Allocator Service is a standalone gRPC microservice that allocates unique, sequential
integer IDs. It replaces the in-process ID generation currently embedded in two existing
modules with a centralized service that multiple independent consumers can share:

- `delivery/delivery_backend/lib/id_generation.js` — uses Google Cloud Datastore as its
  backing store.
- `dashboard/backend/id_allocator.js` — uses Redis (`INCRBY` for atomic increment) as its
  backing store.

Both implementations use the same double-buffered block allocation pattern and the same ID bit
layout (11 reserved + 13 bucket + 40 sequential). They differ only in the backing store and
minor configuration (block size, high-water mark, key format).

The service manages **partitions** — independent ID namespaces. Within a partition, every ID is
allocated at most once. IDs that go unused (e.g., from partially consumed blocks) are never
reclaimed; the allocator treats them as permanently spent.

The system has three layers:

1. **Backing store** — persistent storage that tracks how many IDs have been allocated per
   bucket within each partition (currently Google Cloud Datastore).
2. **ID Allocator Service** — a gRPC service that allocates super blocks from the backing
   store and hands out smaller blocks to clients.
3. **Client library** — an in-process library that requests blocks from the service and
   exposes a simple `allocate_id()` interface to application code.

The service will be written in **C++**. The first client library is **Node.js** (since both
initial consumers are Node.js services). A C++ client library may follow in the future.

## Goals

- Consolidate the two existing ID generation implementations (delivery backend and dashboard
  backend) into a single service.
- Preserve the double-buffered, ahead-of-time allocation pattern that keeps ID allocation
  non-blocking in the common case.
- Support multiple independent ID namespaces (partitions) within a single service instance.
- Provide a clean storage abstraction so the backing store can be swapped or mocked without
  changing service logic.
- Expose a gRPC API for block allocation and partition management.

## Non-Goals

- ID reclamation or garbage collection of unused IDs.
- Globally ordered IDs across partitions.
- Strong consistency guarantees on ID ordering across concurrent clients (IDs are unique, not
  strictly sequential across callers).
- Custom per-partition bit layouts. The ID format is fixed; block sizes are configurable.

## Architecture

```
┌─────────────┐      gRPC       ┌──────────────────┐     Datastore TX     ┌────────────┐
│ Client Lib  │ ──────────────► │  ID Allocator    │ ───────────────────► │  Backing   │
│ (Node / C++)│  AllocateBlock  │  Service (C++)   │  Atomic increment    │  Store     │
│             │ ◄────────────── │                  │ ◄─────────────────── │ (Datastore)│
│ [front|back]│   block range   │ [super blocks]   │   new counter value  │            │
└─────────────┘                 └──────────────────┘                      └────────────┘
```

### Directory Structure

```
id-allocator/
├── PRD.md
├── proto/                      # gRPC / Protobuf definitions
│   └── id_allocator.proto
├── service/                    # ID Allocator Service (C++)
│   └── ...
└── id-allocator-client/        # Node.js client library
    └── ...
```

## Concepts

### Partition

A partition is an independent ID namespace. Partitions are isolated from each other — the same
numeric ID can exist in two different partitions without conflict. Each partition has its own
set of buckets and configuration.

Partition configuration includes:

| Field              | Type     | Description                                          |
|--------------------|----------|------------------------------------------------------|
| `partition_id`     | string   | Unique identifier for the partition.                 |
| `num_buckets`      | uint32   | Number of active buckets to distribute across.       |
| `bucket_size_bits` | uint32   | Number of bits for the sequential portion of the ID. |
| `super_block_size` | uint32   | Number of IDs the service allocates from the store at once. |
| `block_size`       | uint32   | Number of IDs handed out to a client per request.    |

A partition's `super_block_size` must be an exact multiple of its `block_size`.

### Bucket

Within a partition, the ID space is divided into buckets. Each bucket has an independent
counter in the backing store. When allocating a super block, the service picks a bucket
(using the randomized least-loaded strategy from the current implementation), atomically
increments its counter, and takes ownership of that range.

### Super Block

A super block is a contiguous range of IDs that the service allocates from the backing store
in a single transaction. The service holds the super block in memory and carves it into
smaller blocks to serve client requests. The service uses a double-buffer strategy at this
level: when the active super block is partially depleted, a new one is fetched in the
background.

### Block

A block is a contiguous range of IDs that the service hands out to a single client in
response to an `AllocateBlock` RPC. The client library manages its own double buffer of
blocks.

### ID Format

The default format (matching the current delivery backend) is:

```
  63                 52   51            40   39                              0
  ┌──────────────────┬────────────────┬──────────────────────────────────────┐
  │  reserved (11)   │ bucket (13)    │  sequential (40)                     │
  └──────────────────┴────────────────┴──────────────────────────────────────┘
```

- **Reserved (11 bits)**: Keeps IDs within the 53-bit safe integer range for JavaScript.
- **Bucket (13 bits)**: Identifies which bucket the ID was allocated from.
- **Sequential (40 bits)**: Counter within the bucket.

The bit layout is fixed per partition based on `bucket_size_bits` and the number of bucket
index bits implied by `num_buckets`. The service does not expose bit layout as a
user-configurable field — it is derived from the partition parameters.

## gRPC API

### Service: `IdAllocator`

#### `AllocateBlock`

Allocates a block of IDs from a partition.

```protobuf
message AllocateBlockRequest {
  string partition_id = 1;
}

message AllocateBlockResponse {
  uint64 range_start = 1;  // first usable ID (inclusive)
  uint64 range_end = 2;    // last usable ID (exclusive)
}

rpc AllocateBlock(AllocateBlockRequest) returns (AllocateBlockResponse);
```

The returned range `[range_start, range_end)` contains exactly `block_size` IDs for the
requested partition.

**Error conditions:**

| gRPC Status          | Condition                                      |
|----------------------|------------------------------------------------|
| `NOT_FOUND`          | Partition does not exist.                      |
| `RESOURCE_EXHAUSTED` | Bucket rollover — ID space in the partition is near exhaustion. |
| `UNAVAILABLE`        | Transient failure in the backing store.        |

### Service: `IdAllocatorAdmin`

Partition lifecycle management. This is a separate gRPC service to allow independent access
control.

#### `CreatePartition`

```protobuf
message CreatePartitionRequest {
  string partition_id = 1;
  uint32 num_buckets = 2;
  uint32 bucket_size_bits = 3;
  uint32 super_block_size = 4;
  uint32 block_size = 5;
}

message CreatePartitionResponse {}

rpc CreatePartition(CreatePartitionRequest) returns (CreatePartitionResponse);
```

**Error conditions:**

| gRPC Status          | Condition                                                |
|----------------------|----------------------------------------------------------|
| `ALREADY_EXISTS`     | A partition with this ID already exists.                 |
| `INVALID_ARGUMENT`   | Invalid configuration (e.g., super_block_size not a multiple of block_size, num_buckets is 0). |

#### `GetPartition`

```protobuf
message GetPartitionRequest {
  string partition_id = 1;
}

message GetPartitionResponse {
  string partition_id = 1;
  uint32 num_buckets = 2;
  uint32 bucket_size_bits = 3;
  uint32 super_block_size = 4;
  uint32 block_size = 5;
}

rpc GetPartition(GetPartitionRequest) returns (GetPartitionResponse);
```

#### `DeletePartition`

```protobuf
message DeletePartitionRequest {
  string partition_id = 1;
}

message DeletePartitionResponse {}

rpc DeletePartition(DeletePartitionRequest) returns (DeletePartitionResponse);
```

Deleting a partition does not reclaim IDs. It removes the partition configuration; any
in-flight blocks already allocated to clients remain valid but no new blocks can be
allocated.

## Storage Abstraction

The service interacts with the backing store through an abstract interface. This allows:

- Unit testing with an in-memory mock store.
- Swapping to a different backing store (e.g., PostgreSQL, Redis) without changing service
  logic.

The interface (expressed conceptually):

```
interface BlockStore {
  // Atomically increment the counter for a bucket in a partition.
  // Returns the counter value *before* the increment.
  // Throws if the increment would cause rollover past max_value.
  allocate(partition_id, bucket_index, increment, max_value) -> previous_count

  // Read current counters for a set of buckets in a partition.
  // Returns a map of bucket_index -> current_count.
  // Missing buckets are returned with count 0.
  get_bucket_counts(partition_id, bucket_indices) -> map<bucket_index, count>

  // Partition metadata CRUD.
  save_partition(partition_config) -> void
  get_partition(partition_id) -> partition_config | null
  delete_partition(partition_id) -> void
  list_partitions() -> list<partition_config>
}
```

The initial implementation will use Google Cloud Datastore with entity kinds scoped per
partition (e.g., kind `IdBucket:{partition_id}`, key `bucket-{index}`).

Note: the two existing consumers use different backing stores (Datastore and Redis). Once
migrated to the centralized service, the backing store is the service's concern — clients
no longer interact with storage directly. The storage abstraction also makes it
straightforward to add new implementations if needed in the future.

## Client Library

### Behavior

The client library provides a single method to application code:

```
allocate_id() -> uint64
```

Each client instance is bound to a single partition (specified at construction time).
Internally, the client maintains a **double buffer of blocks**:

- **Front block**: The active block from which IDs are allocated.
- **Back block**: A prefetched block ready to swap in when the front is exhausted.

When the front block reaches a **high-water mark** (configurable, e.g., when fewer than N IDs
remain), the client initiates a background gRPC call to fetch a new block into the back
buffer. When the front block is fully consumed, the client swaps front and back.

If both buffers are exhausted (e.g., the service is unreachable for an extended period), the
client returns an error.

### Retry Policy

The client uses exponential backoff with jitter when the service is unreachable. Since block
fetches happen ahead of time (triggered by the high-water mark), transient outages of the
service are masked from application code in the common case.

### Configuration

| Field                | Type     | Description                                          |
|----------------------|----------|------------------------------------------------------|
| `service_address`    | string   | gRPC endpoint of the ID Allocator Service.           |
| `partition_id`       | string   | Which partition to allocate from.                    |
| `high_water_mark`    | uint32   | Remaining IDs in front block that triggers prefetch. Default `1000`. |
| `retry.max_retries`  | uint32   | Maximum number of retry attempts for gRPC calls. Default `5`. |
| `retry.base_delay_ms`| uint32   | Initial backoff delay in milliseconds. Default `100`. |
| `retry.max_delay_ms` | uint32   | Maximum backoff delay in milliseconds. Default `10000`. |
| `channel_credentials`| object   | gRPC channel credentials. Defaults to insecure.      |

### Node.js Client

The Node.js client will use `@grpc/grpc-js` for the gRPC transport. It will expose:

```js
const { IdAllocatorClient } = require('id-allocator-client');

const client = new IdAllocatorClient({
  service_address: 'id-allocator.internal:50051',
  partition_id: 'delivery_request_ids',
  high_water_mark: 1000,
});

await client.initialize();        // connects and fetches the first block
const id = client.allocate_id();  // synchronous, from local buffer
client.close();                   // tears down the gRPC channel
```

`allocate_id()` is synchronous — it reads from the local buffer. The asynchronous gRPC calls
to refill the buffer happen in the background, triggered by the high-water mark.

## Horizontal Scaling

The service can run as multiple replicas. Each replica independently allocates super blocks
from the backing store. Isolation is guaranteed by the transactional semantics of the backing
store (Datastore transactions ensure atomic counter increments). Different replicas may
allocate from different buckets or the same bucket at different counter offsets — both are
safe.

No coordination is required between replicas beyond what the backing store provides.

## Authentication and Network Security

The service will run as a private endpoint within the VPC of the consuming services. For
additional security, Google Cloud's built-in authentication (e.g., service account identity)
can be layered on. The specific auth mechanism is a deployment concern, not a service design
concern.

The admin API (`IdAllocatorAdmin`) should be restricted to authorized operators and not
exposed to regular client services.

## Consumer Migration

Two existing consumers need to migrate to the centralized service. Each gets its own
partition — there is no need to merge their ID spaces.

### Consumer 1: Delivery Backend

Source: `delivery/delivery_backend/lib/id_generation.js`
Current backing store: Google Cloud Datastore (kind `RequestIDBlock`, keys `shard-{index}`)

#### Partition Configuration

| Field              | Value                    |
|--------------------|--------------------------|
| `partition_id`     | `delivery_request_ids`   |
| `num_buckets`      | `128`                    |
| `bucket_size_bits` | `40`                     |
| `super_block_size` | `65536`                  |
| `block_size`       | `2048`                   |

#### Migration Steps

1. **Deploy the ID Allocator Service** with the partition above.
2. **Seed counters** by reading each bucket's current counter from the existing Datastore
   entities (`RequestIDBlock/shard-{index}`) and writing them into the new service's backing
   store with a generous margin (e.g., current value + 1M). Alternatively, reuse the existing
   Datastore entities directly if the new service's storage layer can be configured to match
   the existing kind/key scheme.
3. **Integrate the Node.js client** into the delivery backend, replacing the direct
   `require('./id_generation')` calls.
4. **Cut over** by deploying the updated delivery backend. Since IDs can skip, a brief overlap
   is safe as long as counter seeding prevents collisions.
5. **Decommission** the old `id_generation.js` module and its direct Datastore access.

### Consumer 2: Dashboard Backend

Source: `dashboard/backend/id_allocator.js`
Current backing store: Redis (`INCRBY` with binary key `[0xF0, bucket_index]`)

#### Partition Configuration

| Field              | Value                    |
|--------------------|--------------------------|
| `partition_id`     | `dashboard_ids`          |
| `num_buckets`      | `128`                    |
| `bucket_size_bits` | `40`                     |
| `super_block_size` | `65536`                  |
| `block_size`       | `2048`                   |

#### Migration Steps

1. **Create the partition** via the admin API.
2. **Seed counters** by reading each bucket's current counter from Redis (keys
   `[0xF0, bucket_index]`, values are 53-bit integers) and writing them into the service's
   backing store with a safety margin.
3. **Integrate the Node.js client** into the dashboard backend, replacing the direct
   `require('./id_allocator')` calls.
4. **Cut over** by deploying the updated dashboard backend.
5. **Decommission** the old `id_allocator.js` module and its direct Redis access for ID
   allocation.

### Migration Risk

Low for both consumers. The ID space is large enough that even aggressive counter seeding
(jumping forward by millions) has negligible impact. The primary risk is a misconfigured
counter seed that overlaps with existing allocations — mitigated by reading current counters
and adding a generous margin. The two consumers are independent partitions, so they cannot
collide with each other.

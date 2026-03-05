# id-allocator-client-cpp

C++ client library for the ID Allocator gRPC service.

## API

- `id_allocator::client::IdAllocatorClient`
  - `initialize()`
  - `allocate_id()`
  - `close()`

`allocate_id()` is synchronous in the common case and serves IDs from a local double buffer.

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Dockerized workflow

Use the same C++ toolchain container flow as `id-allocator-service`:

```bash
make toolchain
make test PRESET=debug
```

## Example

```cpp
#include "id_allocator_client.h"

id_allocator::client::IdAllocatorClient client({
    .service_address = "localhost:50051",
    .partition_id = "delivery_request_ids",
    .high_water_mark = 1000,
});

client.initialize();
const uint64_t id = client.allocate_id();
client.close();
```

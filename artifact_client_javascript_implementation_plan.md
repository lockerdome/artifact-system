# Artifact Client JS — Implementation Plan

A Node.js client library that wraps the four artifact-layer gRPC services behind a
high-level, Firebase-style API. Transactions auto-commit on success and auto-rollback
on failure. Snapshot and transaction IDs are kept internal except where needed for
server RPC composition.

The implementation lives at `artifact-system/artifact-client-js/` and follows the same
conventions as `id-allocator/id-allocator-client/` (pure JavaScript, `@grpc/grpc-js`,
`@grpc/proto-loader`, `node:test` for testing).

## Prerequisites

- The artifact-layer gRPC server (P9) is built and runnable.
- Proto files exist at `artifact-system/artifact-layer/proto/` (`artifact_system.proto`,
  `artifact_service.proto`).

## API Design

The client enforces consistent reads by design: **all reads go through a `Snapshot` or
`Transaction` object** — there are no read methods on the client itself. This prevents
accidental inconsistent reads across multiple calls against a moving canonical branch head.

Writes are only available on `Transaction` objects, which are scoped to the async callback
passed to `client.transaction()`. The `Transaction` object becomes inert (all methods
throw) once the callback settles, preventing use-after-commit/rollback bugs.

### Client construction and lifecycle

```javascript
const { ArtifactClient } = require('artifact-client-js');
const grpc = require('@grpc/grpc-js');
const { GoogleAuth } = require('google-auth-library');

// Production: SSL transport + Google Auth call credentials
const auth = new GoogleAuth();
const google_credential = await auth.getClient();
const channel_credentials = grpc.credentials.combineChannelCredentials(
  grpc.credentials.createSsl(),
  grpc.credentials.createFromGoogleCredential(google_credential),
);

const client = new ArtifactClient({
  service_address: 'host:port',
  retry: { max_retries: 5, base_delay_ms: 100, max_delay_ms: 10000 },
  channel_credentials,
});

// Local development only:
// channel_credentials: grpc.credentials.createInsecure()

await client.initialize(); // connect gRPC channels
client.close();            // tear down
```

The client exposes two factory methods: `snapshot()` and `transaction()`.
Reads, writes, and type-registry operations are only available on scoped
`Snapshot`/`Transaction` objects.

### Snapshots (consistent point-in-time reads)

`Snapshot` is the primary read interface. All read operations live here.

```javascript
const snapshot = await client.snapshot();

const a1 = await snapshot.get(artifact_id_1);
// => { artifact_id, type_name, version_id, payload }
// `payload` is a decoded JS object whose field names match the artifact
// type's `.proto` source — NOT raw bytes.  See "Payload decoding and
// encoding" below.

const a2 = await snapshot.get(artifact_id_2); // guaranteed consistent with a1

const artifacts = await snapshot.batch_get([id_1, id_2, id_3]);
// => [{ artifact_id, ..., payload: <decoded object> }, null, ...]

const index = await snapshot.fetch_index(key_type, key_object);
// `key_object` is a JS object matching the index's key message; the
// client encodes it to bytes via the index schema.
// => { index_payload, index_message_name }
// `index_payload` is the decoded index message as a JS object.
```

Snapshots are long-lived and can be used freely after creation. They represent a frozen
point-in-time view.

### Transactions (Firebase-style auto-commit / auto-rollback)

`Transaction` extends the read interface with write methods. It is only accessible
inside the async callback passed to `client.transaction()`.

```javascript
const result = await client.transaction(async (txn) => {
  // Reads within the transaction see snapshot isolation + own writes.
  // `existing.payload` is a decoded JS object.
  const existing = await txn.get(artifact_id);

  // Write operations — only available on txn, not on snapshot or client.
  // `payload_object` and `new_payload_object` are JS objects matching the
  // artifact type's `.proto`; the client encodes them to bytes via the
  // shared TypeRegistryCache before sending.
  const created = await txn.create(version_id, payload_object);
  await txn.update(artifact_id, version_id, new_payload_object);
  await txn.delete(artifact_id);

  // Snapshot from transaction state (for handing to other code that only needs reads)
  const snap = await txn.snapshot();

  // Return value is passed through
  return created.artifact_id;
});
// result => { snapshot_id: '...', value: <return value from callback> }
// If the callback throws, the transaction is rolled back and the error is re-thrown.
```

The `transaction()` wrapper handles the full lifecycle:
1. `CreateTransaction` → gets a transaction_id (ephemeral branch).
2. Execute the user's callback, passing a `Transaction` object whose read/write
   methods automatically scope to the transaction_id.
3. On callback success: `CommitTransaction` → return `{ snapshot_id, value }`.
4. On callback failure: `RollbackTransaction` → re-throw the original error.
5. If rollback itself fails, wrap both errors in an `AggregateError`.
6. **Mark the `Transaction` as settled** — all subsequent method calls throw
   `TransactionSettledError`.

**`txn` is dead after the callback settles.** If a caller leaks the `txn` reference
outside the callback (e.g., stores it in a closure or outer variable) and later calls
a method on it, the call throws `TransactionSettledError`. This is enforced by a
`_settled` flag checked at the top of every method.

**Nested transactions**: `txn.transaction(callback)` creates a sub-transaction
(parent_transaction_id = current txn). The sub-transaction commits into the parent
on callback success and rolls back on failure, without affecting the parent. The
sub-transaction's `txn` object follows the same settled-after-callback rule.

```javascript
await client.transaction(async (txn) => {
  await txn.transaction(async (subtxn) => {
    // Do something in subtxn
  });
});
```

### Type registry

Type registry operations are scoped like artifact CRUD:
- `register_type(...)` is write-like and only available on `Transaction`.
- Read-only type-registry methods are available on readable scopes (`Snapshot`
  and `Transaction`): `get_type_version(...)`, `list_type_versions(...)`,
  `get_index_schema(...)`.

Type-registry RPCs accept transaction or snapshot context the same way CRUD/read
RPCs do, based on whether the operation is write-like or read-only.

```javascript
const snapshot = await client.snapshot();
const version = await snapshot.get_type_version(version_id);
const versions = await snapshot.list_type_versions(type_name);
const schema = await snapshot.get_index_schema(key_type);

await client.transaction(async (txn) => {
  await txn.register_type(type_name, proto_source, {
    deny_create: false,
    deny_update: false,
    deny_delete: false,
  });

  // Read-only type registry calls are also valid on txn
  await txn.get_type_version(version_id);
});
```

### Error classes

All errors extend a base `ArtifactError` class. gRPC error details are parsed and
attached as structured properties.

| Class | Wraps | Source RPCs |
|-------|-------|-------------|
| `ArtifactNotFoundError` | `ArtifactNotFoundError` detail | Get, BatchGet |
| `WriteValidationError` | `ArtifactWriteError` detail (violations array) | Create, Update, Delete |
| `ConflictError` | `CommitConflict` detail (conflict_type, retryable, attempts) | CommitTransaction |
| `TransactionError` | `SnapshotTransactionError` detail (category) | Create/Commit/RollbackTransaction, CreateSnapshot |
| `TransactionSettledError` | (no gRPC detail — client-side guard) | Any method on a settled txn |
| `IndexFetchError` | `FetchIndexError` detail (category) | FetchIndex |
| `TypeRegistrationError` | `RegisterTypeVersionError` detail (violations array) | RegisterTypeVersion |

Each error class exposes the parsed detail message as a `.detail` property so callers
can inspect conflict types, violation categories, etc.

## Deliverables

1. **Directory structure**
   ```
   artifact-system/artifact-client-js/
     package.json
     proto/
       artifact_service.desc # protoc-compiled descriptor set, includes
                             # artifact_service.proto AND artifact_types.proto
                             # (the latter so the client can decode
                             #  TypeDefinition for version_id → type_name)
     lib/
       index.js             # exports { ArtifactClient }
       client.js            # ArtifactClient class — public API + transaction() wrapper
       grpc_client.js       # raw gRPC transport (4 service stubs, proto loading)
       snapshot.js          # Snapshot class (scoped reads, decodes payloads)
       transaction.js       # Transaction class (scoped reads + writes,
                            #   encodes payloads on writes)
       type_registry.js     # TypeRegistryCache: descriptor-set caching +
                            #   payload encode/decode + index key encode
       errors.js            # custom error classes + gRPC detail parsing
       retry.js             # exponential backoff with jitter (same pattern as id-allocator)
     tests/
       mock_server.js       # in-process gRPC server implementing all 4 services,
                            #   with auto-installed fixture artifact type and
                            #   index schema (real descriptor sets)
       client_test.js       # ArtifactClient unit tests
       transaction_test.js  # transaction lifecycle, auto-commit, auto-rollback
       snapshot_test.js     # snapshot scoped reads
       type_registry_test.js # decode/encode pipeline + cache behavior
       errors_test.js       # error parsing and detail extraction
   ```

2. **`lib/grpc_client.js`** — gRPC transport layer:
   - Load all 4 proto files via `@grpc/proto-loader` with `{ keepCase: true, longs: String,
     enums: String, defaults: true, oneofs: true }`.
     Note: `longs: String` — artifact IDs are uint64 which exceed JavaScript's safe integer
     range. The client represents them as strings at the API boundary. Callers pass and
     receive ID values as strings (e.g., `"12345"`). The gRPC layer handles string ↔ proto
     uint64 conversion transparently.
   - Create stubs for `SnapshotTransactionService`, `ArtifactService`, `IndexService`,
     `TypeRegistryService` on a shared gRPC channel.
   - Promisify all unary RPCs (callback → Promise).
   - Extract gRPC error detail messages (`grpc-status-details-bin` trailer) and return
     parsed detail objects alongside gRPC status.

3. **`lib/errors.js`** — error classes and detail parsing:
   - `parse_grpc_error(grpcError)` — decode `grpc-status-details-bin` metadata trailer,
     match detail message types (`ArtifactWriteError`, `CommitConflict`,
     `ArtifactNotFoundError`, `SnapshotTransactionError`, `FetchIndexError`,
     `RegisterTypeVersionError`), and throw the appropriate typed error.
   - Each error class: `message`, `code` (gRPC status code), `detail` (parsed proto
     detail), `grpc_error` (original).
   - `TransactionSettledError` (no gRPC backing) — see `lib/transaction.js`.
   - `TypeDecodeError` (no gRPC backing) — thrown by the type registry when a
     payload cannot be decoded or encoded against its declared type.

4. **`lib/type_registry.js`** — `TypeRegistryCache` class:
   - One instance per `ArtifactClient`, shared with every `Snapshot` and
     `Transaction` (including snapshots/sub-transactions derived from a
     transaction).
   - Resolves a `version_id` to a `protobufjs.Root` by reading
     `GetTypeVersion(version_id).descriptor_set` and rebuilding via
     `protobuf.Root.fromDescriptor(...)`.  Resolves the message's
     `type_name` by reading the parent `TypeDefinition` artifact at
     `type_id` and decoding its payload with the bundled built-in
     `TypeDefinition` Type.
   - Resolves an index `key_type` to its key/value/index Types by reading
     `GetIndexSchema(key_type).index_descriptor_set`.
   - Caches: `version_id → Root`, `version_id → type_name`,
     `type_id → type_name`, `key_type → { root, key/index/value Types,
     key_message_name, index_message_name }`.  All caches are
     process-lifetime; immutability of versions/types makes invalidation
     unnecessary.  Index cache will need an eviction story when index
     migrations land — TODO marker present.
   - Public API used by Snapshot/Transaction:
     - `decode_artifact_payload(version_id, type_name, bytes, read_context)`
       → JS object
     - `encode_artifact_payload(version_id, payload_object, read_context)`
       → `{ type_name, payload_bytes }`
     - `encode_index_key(key_type, key_object, read_context)` → bytes
     - `decode_index_payload(key_type, index_message_name, bytes, read_context)`
       → JS object
   - Throws `TypeDecodeError` on missing-message-in-descriptor, malformed
     descriptors, or payload objects that fail to encode against the
     declared type.
   - All cache-miss RPC calls propagate the caller's `read_context` so a
     transaction that registers a type and immediately uses it within the
     same callback can resolve the new version.
   - See "Payload decoding and encoding" below for the rationale and the
     proto-loader/protobufjs interop quirks the cache handles internally.

5. **`lib/snapshot.js`** — `Snapshot` class (primary read interface):
    - Constructor: `new Snapshot(grpc_client, type_registry, snapshot_id)`.
    - Internal state: `_snapshot_id` only (no public `id` accessor).
    - Methods: `get(artifact_id)`, `batch_get(artifact_ids)`,
      `fetch_index(key_type, key_object)`, `get_type_version(version_id)`,
      `list_type_versions(type_name)`, `get_index_schema(key_type)`.
   - All methods construct a `ReadContext { snapshot_id }` and delegate to grpc_client.
   - **Decoding**: `get` / `batch_get` pass the response payload bytes through
     `type_registry.decode_artifact_payload(...)` and return the resulting
     JS object as the artifact's `payload` field.
   - **Index decoding/encoding**: `fetch_index` accepts a JS key object,
     encodes it via `type_registry.encode_index_key(...)`, and decodes the
     response `index_payload` via `type_registry.decode_index_payload(...)`.
   - `get_type_version` / `list_type_versions` / `get_index_schema`
     propagate `read_context: { snapshot_id }` to the underlying RPC.
   - Long-lived — no settled state, can be used freely after creation.

6. **`lib/transaction.js`** — `Transaction` class (reads + writes, scoped to callback):
    - Constructor: `new Transaction(grpc_client, type_registry, transaction_id)`.
    - Internal state: `_transaction_id` only (no public `id` accessor).
   - Internal: `_settled` flag, initially `false`.
   - **Settled guard**: every public method checks `_settled` first and throws
     `TransactionSettledError` if true. `_settle()` is called by the
     `client.transaction()` wrapper after the callback resolves or rejects.
    - Read methods: `get(artifact_id)`, `batch_get(artifact_ids)`,
      `fetch_index(key_type, key_object)`, `get_type_version(version_id)`,
      `list_type_versions(type_name)`, `get_index_schema(key_type)` — scoped to
      `ReadContext { transaction_id }`.  Reads decode payloads / index
      payloads through the shared `type_registry` (same flow as `Snapshot`).
    - Write methods: `create(version_id, payload_object)`,
      `update(artifact_id, version_id, payload_object)`,
      `delete(artifact_id)`, `register_type(type_name, proto_source, options)`.
      `create` / `update` encode the JS payload object via
      `type_registry.encode_artifact_payload(...)` before sending; the
      caller never sees raw bytes.  All write RPCs pass `transaction_id`,
      including `register_type`.
    - `snapshot()` — create a snapshot from the transaction's current state
      (parent_transaction_id = this._transaction_id). Returns a `Snapshot`
      constructed with the same `type_registry` so it remains usable after
      the transaction settles.
    - `transaction(callback)` — nested sub-transaction
      (parent_transaction_id = this._transaction_id).
      Uses the same auto-commit/rollback/settle pattern as `client.transaction()`,
      and the sub-transaction shares the parent's `type_registry`.
   - The `Transaction` class does **not** call commit/rollback itself — that is the
     responsibility of the `client.transaction()` wrapper.

7. **`lib/client.js`** — `ArtifactClient` class:
   - Constructor validates options, creates `ArtifactGrpcClient`, and
     creates **one** `TypeRegistryCache` that is shared with every
     `Snapshot` and `Transaction` produced by this client.
   - `initialize()` / `close()` — connection lifecycle.
    - **No read methods** — reads go through `Snapshot` or `Transaction`.
    - **No write methods** — writes go through `Transaction`.
    - **No type-registry methods** — registry calls are scoped through
      `Snapshot`/`Transaction`.
   - **Snapshot factory**: `snapshot()` — calls `CreateSnapshot` (canonical
     branch head), returns a `Snapshot` constructed with the shared
     `type_registry`.
   - **Transaction wrapper**: `transaction(callback, options?)`:
     1. `CreateTransaction(options)` → transaction_id
     2. Construct `Transaction` with the shared `type_registry`
     3. `try { value = await callback(txn); }` → `CommitTransaction` → return
        `{ snapshot_id, value }`
     4. `catch (err)` → `RollbackTransaction` → re-throw
     5. `finally` → `txn._settle()` — mark txn as inert

8. **`tests/mock_server.js`** — in-process gRPC test server:
   - Implements all 4 services with in-memory state.
   - Configurable failure injection (specific RPCs can return errors).
   - Tracks call history for assertions.
   - **Fixture types**: at construction, compiles a fixture artifact type
     (`test.TestPayload`) and a fixture index schema
     (`IndexKey_Test` / `IndexValue_Test` / `Index_Test`) into real
     `FileDescriptorSet`s, auto-installs a `TypeDefinition` artifact at the
     fixture's `type_id`, and serves them from `GetTypeVersion` /
     `GetIndexSchema` / `GetArtifact` so the client can run its full
     decode/encode pipeline against the mock.  Exposes `test_version_id`,
     `test_type_name`, `test_index_key_type`, etc. for tests.
   - `seed_artifact(id, payload)` and `seed_index(key, index_payload)`
     accept JS objects (encoded via the fixture types) or raw bytes.

9. **`tests/client_test.js`** — ArtifactClient tests:
    - `client.snapshot()` returns a working `Snapshot`
    - Error handling (type registration errors, etc.)
    - Verify client has no `get`, `batch_get`, `create`, `update`, `delete`,
      `register_type`, `get_type_version`, `list_type_versions`, `get_index_schema`
      methods

10. **`tests/transaction_test.js`** — transaction lifecycle tests:
   - Auto-commit on callback success
   - Auto-rollback on callback throw
   - Return value passthrough via `result.value`
   - `result.snapshot_id` available for read-after-write
   - Reads within transaction (get, batch_get, fetch_index) — assert that
     the returned `payload` is a decoded JS object matching the seeded data
    - Writes within transaction (create, update, delete) — pass JS payload
      objects, not bytes
    - Type registry in transaction (`register_type`) and readable registry methods
      on `txn`
   - Nested transactions (`txn.transaction(async (subtxn) => { ... })`)
   - `ConflictError` on commit conflicts
   - Rollback failure handling (AggregateError)
   - **Settled enforcement**: calling any method after callback settles throws
     `TransactionSettledError`
   - Snapshot created from txn (`txn.snapshot()`) remains usable after txn settles

11. **`tests/snapshot_test.js`** — snapshot tests:
    - Snapshot from canonical branch (`client.snapshot()`)
    - Snapshot from transaction (`txn.snapshot()` inside callback)
    - All read methods: get, batch_get, fetch_index — assert decoded
      payload fields, not just metadata
    - Read-only type registry methods: get_type_version, list_type_versions,
      get_index_schema
    - Snapshot does not expose `snapshot_id` via a public `id` field
    - Snapshot remains usable indefinitely after creation
    - `ArtifactNotFoundError` for missing artifacts
    - `IndexFetchError` for bad index queries

12. **`tests/type_registry_test.js`** — decode/encode pipeline + cache tests:
    - Decoded payloads come back as JS objects matching the `.proto` field
      names verbatim (no normalization in either direction)
    - Round-trip through `txn.create` → `snapshot.get` returns the original
      payload
    - `batch_get` decodes every entry
    - `version_id → Type` cache: `GetTypeVersion` is called exactly once
      across multiple reads of the same version (and across `Snapshot` /
      `Transaction` from the same client)
    - `key_type → schema` cache behaves the same way for `GetIndexSchema`
    - Index keys encode deterministically so seeded entries match lookups
    - Encoding a malformed payload throws `TypeDecodeError`
    - Uses a fresh `ArtifactClient` per test so cache state is assertable

13. **`tests/errors_test.js`** — error parsing tests:
    - Each error detail type correctly parsed from gRPC metadata
    - `TransactionSettledError` thrown client-side (no gRPC detail)
    - Unknown detail types don't crash
    - Missing metadata falls back to generic error

## Payload decoding and encoding

Artifact and index payloads are protobuf-encoded bytes on the wire, but the
client API exposes them as JS objects. A `TypeRegistryCache` (instantiated
once per `ArtifactClient` and shared with every `Snapshot` / `Transaction`)
resolves message types from descriptor sets returned by the server and
performs decode/encode transparently.

**Sources of truth (no client-side `protoc`):**
- Artifact types: `GetTypeVersionResponse.descriptor_set`
  (`google.protobuf.FileDescriptorSet`).
- Index types: `GetIndexSchemaResponse.index_descriptor_set` plus
  `key_message_name` and `index_message_name`.

The client never parses `.proto` text — it consumes binary `FileDescriptorSet`s
that the server has already compiled with `protoc`. This sidesteps protobufjs's
historical issues with parsing custom options in `.proto` source. Custom
options are not part of the payload wire format, so resolving message types via
`protobuf.Root.fromDescriptor(...)` is sufficient for encode/decode.

**version_id → type_name resolution.** `GetTypeVersionResponse` does not
currently carry `type_name`. The client resolves it by reading the parent
`TypeDefinition` artifact (whose `artifact_id == type_id`) using the built-in
`TypeDefinition` message definition. The built-in definition is bundled into
`proto/artifact_service.desc` by adding `artifact_types.proto` to the
`generate-proto` script. Both `version_id → type_name` and
`type_id → type_name` are cached. If a future server change adds `type_name`
directly to `GetTypeVersionResponse`, the extra round trip can be removed.

**Caches** (all process-lifetime, never invalidated):
- `version_id → protobufjs.Root` (versions are immutable)
- `version_id → type_name` (derived once, then memoized)
- `type_id → type_name`
- `key_type → { root, key_type_msg, index_type_msg, key_message_name, index_message_name }`
  — TODO: revisit when index migrations are introduced.

**Encoding direction.** Writes (`txn.create`, `txn.update`) accept JS objects
and encode via the cache. `fetch_index` accepts a JS key object and encodes it
using `key_message_name` from the index schema.

**Field names preserve `.proto` source casing.** No layer in the pipeline
normalizes or converts field names. The client reuses the
`google.protobuf.FileDescriptorSet` Type from `_error_root` (which was built
via `protobuf.Root.fromDescriptor` and therefore takes its field names
verbatim from the binary `FieldDescriptorProto.name` strings — i.e. exactly
as they appear in the `.proto` source). The cache re-encodes the
`descriptor_set` JS object to bytes via that Type and feeds the bytes back
into `protobuf.Root.fromDescriptor(buffer)`. The resulting user-message
Types take their field names from the same source, so decoded payloads
(`toObject(...)`) come back with whatever case the artifact's `.proto`
declared — typically snake_case in this codebase, but the mechanism doesn't
care. `@grpc/proto-loader` is also configured with `keepCase: true`, so
descriptor metadata (`message_type`, `enum_type`, ...) is consistent
across the proto-loader and protobufjs sides without a converter.

**`TypeDecodeError`** is thrown when a referenced message name is not found
in the descriptor set, when verification of an outgoing payload fails, or
when descriptor data is missing/malformed.

## uint64 handling

JavaScript `Number` cannot safely represent uint64 values (max safe integer is 2^53 - 1).
All artifact IDs, version IDs, and type IDs are represented as strings at the client API
boundary. Proto loader is configured with `longs: String` so the gRPC layer returns
string representations of uint64 fields. This is consistent with how most JavaScript gRPC
clients handle uint64 and avoids silent precision loss.

## Checklist
- [ ] Directory structure and `package.json` created
- [ ] `proto/artifact_service.desc` includes `artifact_types.proto`
      (so the client has the built-in `TypeDefinition` available for
      `version_id → type_name` resolution)
- [ ] `lib/retry.js` — exponential backoff with jitter
- [ ] `lib/errors.js` — error classes + gRPC detail parsing (`grpc-status-details-bin`) + `TransactionSettledError` + `TypeDecodeError`
- [ ] `lib/type_registry.js` — `TypeRegistryCache`: version → Root,
      version → type_name (via TypeDefinition), index schemas, and the
      `decode_artifact_payload` / `encode_artifact_payload` /
      `encode_index_key` / `decode_index_payload` helpers
- [ ] `lib/grpc_client.js` — proto loading, 4 service stubs, promisified RPCs
- [ ] `lib/snapshot.js` — Snapshot class; reads decode payloads through `type_registry`
- [ ] `lib/transaction.js` — Transaction class; reads decode and writes
      encode through `type_registry`; settled guard after callback;
      `register_type` passes `transaction_id`
- [ ] `lib/client.js` — ArtifactClient with snapshot() + transaction()
      factories only; instantiates a single shared `TypeRegistryCache`
- [ ] `lib/index.js` — public exports (including `TypeDecodeError`)
- [ ] `tests/mock_server.js` — in-process gRPC server with auto-installed
      fixture artifact type and index schema (real `FileDescriptorSet`s)
- [ ] `tests/client_test.js` — snapshot factory; no read/write/registry methods on client
- [ ] `tests/transaction_test.js` — auto-commit, auto-rollback, settled guard, nested, conflict; payloads are JS objects
- [ ] `tests/snapshot_test.js` — creation, scoped reads, from transaction, long-lived; payloads are JS objects
- [ ] `tests/type_registry_test.js` — decode/encode round-trip, cache
      behavior, `TypeDecodeError` on malformed payloads
- [ ] `tests/errors_test.js` — all error detail types + TransactionSettledError
- [ ] uint64 IDs handled as strings (no precision loss)
- [ ] Artifact and index payloads exposed as JS objects at the API
      boundary (callers never see raw bytes)
- [ ] Field names preserved verbatim from `.proto` source — no casing
      normalization at any layer
- [ ] All tests pass (`node --test tests/*.js`)

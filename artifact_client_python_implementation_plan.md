# Artifact Client Python — Implementation Plan

A Python client library that wraps the four artifact-layer gRPC services behind the same
high-level API as the JavaScript client. Transactions auto-commit on success and
auto-rollback on exception. Snapshot and transaction IDs are surfaced where needed for
consistent reads.

The implementation lives at `artifact-system/artifact-client-python/` and uses
`grpcio` + `grpcio-tools` for gRPC, `pytest` for testing. The API mirrors the JavaScript
client's design principles: no reads/writes on the client, reads through `Snapshot`,
writes through `Transaction`, settled guard on transactions.

Python's `async with` and context manager protocols are a natural fit for the
transaction lifecycle pattern. The library provides both sync and async interfaces.

The code is written in pure Python but is structured for optional Cython compilation
(see Cython optimization section below).

## Prerequisites

- The artifact-layer gRPC server (P9) is built and runnable.
- Proto files exist at `artifact-system/artifact-layer/proto/` (`artifact_system.proto`,
  `artifact_service.proto`).

## API Design

The same design constraints as the JS client apply: **all reads go through a `Snapshot` or
`Transaction` object**, writes only through `Transaction`, and the `Transaction` is
dead after its context exits.

### Client construction and lifecycle

```python
import grpc
import google.auth
import google.auth.transport.grpc as google_auth_grpc
from artifact_client import ArtifactClient

# Production: SSL transport + Google Auth call credentials
credentials, project = google.auth.default()
channel_credentials = grpc.composite_channel_credentials(
    grpc.ssl_channel_credentials(),
    google_auth_grpc.AuthMetadataPlugin(credentials),
)

client = ArtifactClient(
    service_address="host:port",
    retry=RetryOptions(max_retries=5, base_delay_s=0.1, max_delay_s=10.0),
    channel_credentials=channel_credentials,
)

# Local development only:
# channel_credentials=grpc.local_channel_credentials()

# Async variant
client = AsyncArtifactClient(
    service_address="host:port",
    channel_credentials=channel_credentials,
)
```

The client exposes four methods: `snapshot()`, `transaction()`, `register_type()`, and
the type introspection helpers. No reads, no writes.

### Snapshots (consistent point-in-time reads)

`Snapshot` is the primary read interface. All read operations live here.

```python
snapshot = client.snapshot()

a1 = snapshot.get(artifact_id_1)
# => ArtifactRecord(artifact_id, type_name, version_id, payload)

a2 = snapshot.get(artifact_id_2)  # consistent with a1

artifacts = snapshot.batch_get([id1, id2, id3])
# => [ArtifactRecord(...), None, ArtifactRecord(...)]  (None = not found)

index = snapshot.fetch_index(key_type, key_bytes)
# => IndexResult(index_payload, index_message_name)

snapshot.id  # the opaque snapshot_id string (commit hash)
```

Snapshots are long-lived and can be used freely after creation.

```python
# Async variant
snapshot = await client.snapshot()
a1 = await snapshot.get(artifact_id_1)
```

### Transactions (context-manager auto-commit / auto-rollback)

`Transaction` extends the read interface with write methods. It is only accessible
inside a `with` block (sync) or `async with` block (async).

```python
with client.transaction() as txn:
    # Reads within the transaction see snapshot isolation + own writes
    existing = txn.get(artifact_id)

    # Write operations — only available on txn, not on snapshot or client
    created = txn.create(version_id, payload)
    txn.update(artifact_id, version_id, new_payload)
    txn.delete(artifact_id)

    # Snapshot from transaction state
    snap = txn.snapshot()

# If the block exits normally: auto-commit
# txn.result => TransactionResult(snapshot_id, committed=True)

# If the block raises: auto-rollback, exception re-raised
```

```python
# Async variant
async with client.transaction() as txn:
    existing = await txn.get(artifact_id)
    created = await txn.create(version_id, payload)
# auto-commits on clean exit, auto-rollbacks on exception
```

The context manager handles the full lifecycle:
1. `__enter__` / `__aenter__`: `CreateTransaction` → transaction_id, construct
   `Transaction`.
2. `__exit__` / `__aexit__`: if no exception → `CommitTransaction`, store
   `snapshot_id` in `txn.result`. If exception → `RollbackTransaction`, re-raise.
3. If rollback itself fails, raise `TransactionRollbackError` with both the original
   exception and the rollback exception chained via `__cause__`.
4. **Mark the `Transaction` as settled** — all subsequent method calls raise
   `TransactionSettledError`.

**`txn` is dead after the `with` block exits.** If a caller leaks the `txn` reference
and later calls a method on it, the call raises `TransactionSettledError`. Enforced by
a `_settled` flag checked at the top of every method.

**Return values from transactions**: Unlike the JS client's callback which returns a
value, Python's `with` statement doesn't have a return channel. Instead:
- Write methods return their results directly (`txn.create()` returns a
  `CreateResult(artifact_id, snapshot_id)`).
- After a successful commit, `txn.result` contains a `TransactionResult(snapshot_id)`
  for read-after-write.
- Callers collect values from write calls within the block as needed.

**Nested transactions**: `txn.transaction()` returns a context manager for a
sub-transaction (parent_transaction_id = current txn). The sub-transaction commits
into the parent on clean exit and rolls back on exception. The sub-transaction's
`txn` object follows the same settled-after-exit rule.

```python
with client.transaction() as txn:
    created = txn.create(version_id, payload)
    with txn.transaction() as sub_txn:
        sub_txn.update(created.artifact_id, version_id, updated_payload)
    # sub-transaction committed into parent
# parent committed to canonical branch
```

### Type registry

```python
result = client.register_type(type_name, proto_source,
    deny_create=False, deny_update=False, deny_delete=False)
# => RegisterResult(version_id)

version  = client.get_type_version(version_id)
versions = client.list_type_versions(type_name)
schema   = client.get_index_schema(key_type)
```

### Error classes

All errors extend a base `ArtifactError` class (which extends `Exception`). gRPC error
details are parsed and attached as structured attributes.

| Class | Wraps | Source RPCs |
|-------|-------|-------------|
| `ArtifactNotFoundError` | `ArtifactNotFoundError` detail | Get, BatchGet |
| `WriteValidationError` | `ArtifactWriteError` detail (violations list) | Create, Update, Delete |
| `ConflictError` | `CommitConflict` detail (conflict_type, retryable, attempts) | CommitTransaction |
| `TransactionError` | `SnapshotTransactionError` detail (category) | Create/Commit/RollbackTransaction, CreateSnapshot |
| `TransactionSettledError` | (no gRPC detail — client-side guard) | Any method on a settled txn |
| `TransactionRollbackError` | (wraps original + rollback exceptions) | Transaction context exit |
| `IndexFetchError` | `FetchIndexError` detail (category) | FetchIndex |
| `TypeRegistrationError` | `RegisterTypeVersionError` detail (violations list) | RegisterTypeVersion |

Each error class exposes the parsed detail message as a `.detail` attribute.

## Deliverables

1. **Directory structure**
   ```
   artifact-system/artifact-client-python/
     pyproject.toml
     artifact_client/
       __init__.py          # exports ArtifactClient, AsyncArtifactClient
       _client.py           # ArtifactClient + AsyncArtifactClient
       _grpc_client.py      # raw gRPC transport (4 service stubs, sync + async channels)
       _snapshot.py          # Snapshot + AsyncSnapshot
       _transaction.py       # Transaction + AsyncTransaction (context managers)
       _errors.py            # error classes + gRPC detail parsing
       _retry.py             # exponential backoff with jitter
       _types.py             # data classes (ArtifactRecord, CreateResult, TransactionResult, etc.)
       py.typed              # PEP 561 marker
     proto/                  # symlink or copy of artifact-layer/proto/
     tests/
       conftest.py           # pytest fixtures (mock server, client instances)
       mock_server.py        # in-process gRPC server implementing all 4 services
       test_client.py        # ArtifactClient tests
       test_transaction.py   # transaction lifecycle tests
       test_snapshot.py      # snapshot tests
       test_errors.py        # error parsing tests
   ```

2. **Proto code generation**: `pyproject.toml` build step or Makefile target that runs
   `grpc_tools.protoc` to generate `*_pb2.py` and `*_pb2_grpc.py` stubs from the
   proto files. Generated files live in `artifact_client/_generated/` and are
   gitignored (regenerated at build time).

3. **`artifact_client/_grpc_client.py`** — gRPC transport layer:
   - Sync: `grpc.insecure_channel` / `grpc.secure_channel` with generated stubs.
   - Async: `grpc.aio.insecure_channel` / `grpc.aio.secure_channel`.
   - Create stubs for `SnapshotTransactionServiceStub`, `ArtifactServiceStub`,
     `IndexServiceStub`, `TypeRegistryServiceStub`.
   - Extract gRPC error detail messages from `RpcError.trailing_metadata()` using
     `google.rpc.status_pb2.Status` and `google.protobuf.any_pb2.Any.Unpack()`.

4. **`artifact_client/_errors.py`** — error classes and detail parsing:
   - `parse_grpc_error(rpc_error)` — decode trailing metadata, match detail message
     types, raise the appropriate typed error.
   - Each error class: `message`, `code` (gRPC status code), `detail` (parsed proto
     detail), `grpc_error` (original `RpcError`).
   - `TransactionSettledError` — client-side, no gRPC detail.
   - `TransactionRollbackError` — chains original exception via `__cause__`.

5. **`artifact_client/_types.py`** — data classes:
   - `ArtifactRecord(artifact_id: int, type_name: str, version_id: int, payload: bytes)`
   - `CreateResult(artifact_id: int, snapshot_id: str)`
   - `WriteResult(snapshot_id: str)`
   - `TransactionResult(snapshot_id: str)`
   - `IndexResult(index_payload: bytes, index_message_name: str)`
   - `TypeVersion(version_id: int, type_id: int, descriptor_set: bytes, proto_source: str, previous_version_id: int | None, next_version_id: int | None)`
   - `IndexSchema(index_definition_id: int, key_type: str, key_fields: list[str], order_fields: list, unique: bool, index_descriptor_set: bytes, key_message_name: str, value_message_name: str, index_message_name: str)`
   - `RegisterResult(version_id: int)`
   - All use `@dataclass(frozen=True, slots=True)`.
   - uint64 values are Python `int` (arbitrary precision, no loss).

6. **`artifact_client/_snapshot.py`** — `Snapshot` + `AsyncSnapshot`:
   - `__slots__ = ('_grpc_client', '_snapshot_id')`
   - Constructor: `Snapshot(grpc_client: GrpcClient, snapshot_id: str)`.
   - Properties: `id -> str` — the opaque snapshot_id string.
   - Methods: `get(artifact_id: int) -> ArtifactRecord`,
     `batch_get(artifact_ids: list[int]) -> list[ArtifactRecord | None]`,
     `fetch_index(key_type: str, key: bytes) -> IndexResult`.
   - All methods construct a `ReadContext(snapshot_id=...)` and delegate to grpc_client.
   - Long-lived — no settled state.
   - `AsyncSnapshot` mirrors the interface with `async` methods.

7. **`artifact_client/_transaction.py`** — `Transaction` + `AsyncTransaction`:
   - `__slots__ = ('_grpc_client', '_transaction_id', '_settled', 'result')`
   - Implements `__enter__` / `__exit__` (sync) and `__aenter__` / `__aexit__` (async).
   - `_settled: bool` flag, checked by every public method via an inline guard
     (not a decorator — Cython compiles the `if` check to a single branch).
   - `result: TransactionResult | None` — `None` until commit succeeds.
   - Read methods: `get(artifact_id: int) -> ArtifactRecord`,
     `batch_get(artifact_ids: list[int]) -> list[ArtifactRecord | None]`,
     `fetch_index(key_type: str, key: bytes) -> IndexResult` — scoped to
     `ReadContext(transaction_id=...)`.
   - Write methods: `create(version_id: int, payload: bytes) -> CreateResult`,
     `update(artifact_id: int, version_id: int, payload: bytes) -> WriteResult`,
     `delete(artifact_id: int) -> WriteResult`.
   - `snapshot() -> Snapshot` — create from transaction state. The returned `Snapshot`
     outlives the transaction.
   - `transaction() -> Transaction` — nested sub-transaction context manager.
   - Does **not** call commit/rollback itself — the context manager protocol handles it.

8. **`artifact_client/_client.py`** — `ArtifactClient` + `AsyncArtifactClient`:
   - `__slots__ = ('_grpc_client', '_retry_options', '_channel_credentials')`
   - Constructor validates options with explicit parameters (no `**kwargs`), creates
     gRPC channel and stubs.
   - `close() -> None` — tear down channel.
   - **No read methods, no write methods.**
   - `snapshot() -> Snapshot` — create from canonical branch head.
   - `transaction(parent_snapshot_id: str | None = None) -> Transaction` — returns
     context manager. Options are explicit keyword arguments, not `**kwargs`.
   - `register_type(type_name: str, proto_source: str, deny_create: bool = False, deny_update: bool = False, deny_delete: bool = False) -> RegisterResult`
   - `get_type_version(version_id: int) -> TypeVersion`
   - `list_type_versions(type_name: str) -> list[int]`
   - `get_index_schema(key_type: str) -> IndexSchema`
   - `AsyncArtifactClient` mirrors with async methods; `snapshot()` and
     `transaction()` return async variants.

9. **`artifact_client/_retry.py`** — retry utility:
   - `retry_with_backoff(fn, max_retries, base_delay_s, max_delay_s)` — same algorithm
     as JS client (exponential backoff with full jitter).
   - Async variant: `async_retry_with_backoff(...)` using `asyncio.sleep`.

10. **`tests/mock_server.py`** — in-process gRPC test server:
    - Implements all 4 services with in-memory state using `grpc.server(...)`.
    - Configurable failure injection.
    - Tracks call history for assertions.
    - Fixture starts server on a random port, tears down after test.

11. **`tests/test_client.py`** — ArtifactClient tests:
    - `client.snapshot()` returns a working `Snapshot`
    - Type registry operations
    - Verify client has no `get`, `batch_get`, `create`, `update`, `delete` methods
    - `close()` tears down cleanly

12. **`tests/test_transaction.py`** — transaction lifecycle tests:
    - Auto-commit on clean `with` block exit
    - Auto-rollback on exception
    - `txn.result.snapshot_id` available after commit
    - Reads within transaction (get, batch_get, fetch_index)
    - Writes within transaction (create, update, delete)
    - Nested transactions (`with txn.transaction() as sub_txn`)
    - `ConflictError` on commit conflicts
    - `TransactionRollbackError` when rollback fails (chained exceptions)
    - **Settled enforcement**: calling any method after `with` block exits raises
      `TransactionSettledError`
    - Snapshot created from txn remains usable after txn settles

13. **`tests/test_snapshot.py`** — snapshot tests:
    - Snapshot from canonical branch
    - Snapshot from transaction
    - All read methods: get, batch_get, fetch_index
    - `snapshot.id` is the opaque commit hash
    - Snapshot remains usable indefinitely
    - `ArtifactNotFoundError` for missing artifacts
    - `IndexFetchError` for bad index queries

14. **`tests/test_errors.py`** — error parsing tests:
    - Each error detail type correctly parsed from gRPC trailing metadata
    - `TransactionSettledError` raised client-side
    - `TransactionRollbackError` chains original and rollback exceptions
    - Unknown detail types don't crash
    - Missing metadata falls back to generic error

## uint64 handling

Python integers have arbitrary precision, so uint64 values are represented as native
`int` — no string conversion or precision loss. This is simpler than the JS client's
string-based approach.

## Sync vs async

Both sync and async interfaces are provided. The sync client uses `grpc.insecure_channel`
and blocking stubs. The async client uses `grpc.aio` channels and `await`-based stubs.
The core logic (error parsing, retry, data classes) is shared; only the transport and
public method signatures differ.

The naming convention:
- Sync: `ArtifactClient`, `Snapshot`, `Transaction`
- Async: `AsyncArtifactClient`, `AsyncSnapshot`, `AsyncTransaction`

Both are exported from `artifact_client.__init__`.

## Cython optimization

The library is written as pure Python that Cython 3.0+ can compile without
modification, using Cython's pure Python mode. This means the same source runs
interpreted (for development, debugging, and environments where Cython is not
available) and compiled (for production throughput).

**Coding rules enforced across all modules:**

1. **`__slots__` on every class** — `Snapshot`, `AsyncSnapshot`, `Transaction`,
   `AsyncTransaction`, `ArtifactClient`, `AsyncArtifactClient`, `GrpcClient`,
   and all error classes. Dataclasses already use `slots=True`. This gives Cython
   direct struct-member access instead of `__dict__` lookups.

2. **Full type annotations on all function signatures and local variables where the
   type is not obvious from the right-hand side.** Cython 3.0 pure Python mode reads
   standard annotations to generate typed C code. Annotate parameters, return types,
   and any local that holds a primitive (`int`, `bool`, `str`, `bytes`).

3. **No `*args` or `**kwargs` on any public or internal method.** All parameters are
   explicit and positional-or-keyword. Cython cannot optimize argument unpacking.

4. **Settled guard is an inline `if` check, not a decorator.** Decorators add a Python
   function call per invocation. An `if self._settled: raise TransactionSettledError()`
   at the top of each method compiles to a single branch instruction.

5. **No closures in method bodies.** Helper logic is extracted into private methods
   (prefixed with `_`) so Cython can compile them as C-level calls. The retry utility
   accepts a callable parameter rather than closing over state.

6. **Frozen dataclasses with `slots=True`** for all result/record types. Cython
   optimizes attribute access on slotted frozen dataclasses.

7. **`@cython.cclass` compatibility** — class hierarchies are kept flat (no deep
   inheritance chains). Error classes inherit from `ArtifactError` (one level), which
   inherits from `Exception`. Snapshot and Transaction do not share an abstract base
   class; they duplicate the read method signatures independently.
   This is intentional — Cython `cclass` types support single inheritance only.

**Build configuration:**

`pyproject.toml` declares an optional `[build-system]` that uses `setuptools` with
a `build_ext` step. When Cython is available, `*.py` files under `artifact_client/`
(excluding `__init__.py`) are compiled to C extension modules. When Cython is not
installed, the build produces a pure-Python wheel. This is controlled by a
try/except in `setup.py` or a build backend hook:

```python
# setup.py (or equivalent build hook)
try:
    from Cython.Build import cythonize
    ext_modules = cythonize(
        "artifact_client/_*.py",
        compiler_directives={
            "language_level": "3",
            "boundscheck": False,
            "wraparound": False,
        },
    )
except ImportError:
    ext_modules = []
```

The `__init__.py` is **not** compiled — it remains a pure Python re-export module so
that import machinery works regardless of whether extensions are present.

`cython` and `setuptools` are listed as optional build dependencies under
`[build-system.requires]` with a feature flag (e.g., `pip install .[cython]`).

## Checklist
- [ ] Directory structure, `pyproject.toml`, and `py.typed` marker created
- [ ] Proto code generation (Makefile target or build step)
- [ ] `_retry.py` — sync + async exponential backoff with jitter
- [ ] `_errors.py` — error classes + gRPC detail parsing + `TransactionSettledError` + `TransactionRollbackError`
- [ ] `_types.py` — frozen dataclasses for all result types
- [ ] `_grpc_client.py` — proto stub wrappers, sync + async channels
- [ ] `_snapshot.py` — Snapshot + AsyncSnapshot (primary read interface, long-lived)
- [ ] `_transaction.py` — Transaction + AsyncTransaction (context managers, settled guard)
- [ ] `_client.py` — ArtifactClient + AsyncArtifactClient (snapshot, transaction, registry only)
- [ ] `__init__.py` — public exports
- [ ] `tests/mock_server.py` — in-process gRPC server for all 4 services
- [ ] `tests/test_client.py` — snapshot factory, registry, no CRUD on client
- [ ] `tests/test_transaction.py` — auto-commit, auto-rollback, settled guard, nested, conflict, rollback error chaining
- [ ] `tests/test_snapshot.py` — creation, scoped reads, from transaction, long-lived
- [ ] `tests/test_errors.py` — all error detail types + TransactionSettledError + TransactionRollbackError
- [ ] Sync and async interfaces both tested
- [ ] `__slots__` on every class (not just dataclasses)
- [ ] Full type annotations on all signatures and key locals
- [ ] No `*args`/`**kwargs` — all parameters explicit
- [ ] No closures in method bodies — private methods instead
- [ ] No decorators on hot-path methods (settled guard is inline `if`)
- [ ] `pyproject.toml` supports optional Cython build (`pip install .[cython]`)
- [ ] Tests pass in both pure-Python and Cython-compiled modes
- [ ] All tests pass (`pytest`)

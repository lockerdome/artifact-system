# Artifact Layer

The artifact layer is the C++ backend of the artifact system. It implements storage, indexing, type registration, transactions, and referential integrity, exposed over four gRPC services.

For the user-facing API and JS client documentation, see the parent [artifact-system README](../README.md).

## Subsystems

### service/

gRPC service implementations. Each service delegates to one or more business-logic modules:

| Service | Responsibilities |
|---------|-----------------|
| `SnapshotTransactionService` | Create/commit/rollback snapshots and transactions |
| `ArtifactService` | Create, get, batch-get, update, delete artifacts |
| `IndexService` | `FetchIndex` queries against secondary indexes |
| `TypeRegistryService` | Register type versions, introspect types and index schemas |

`server.cpp` wires these services together and starts the gRPC server.

### storage/

Pluggable storage backend behind `StorageInterface` (branch-based object store with merge semantics).

| Implementation | Purpose |
|----------------|---------|
| `MemoryStorage` | In-process, used by all unit tests |
| `LakeFsStorage` | HTTP client to a LakeFS server, used in production |

### transaction/

Snapshot and transaction lifecycle management.

- **TransactionManager** -- creates snapshots and transactions, manages commit and rollback.
- **WriteExecutor** -- merges a transaction branch into the canonical branch with exponential-backoff retry on retryable conflicts.
- **ConflictResolver** -- classifies merge conflicts (payload vs. index vs. referential integrity) and decides retryability.

### artifact/

Core CRUD logic and write-time validation.

- **ArtifactStore** -- orchestrates create/update/delete: validates the payload, derives index entries, enforces referential integrity, then delegates to the transaction layer.
- **Validation** -- checks version resolution, mutation-deny flags, payload structure, and field-level constraints (e.g. NaN rejection in indexed floats).
- **ReferentialIntegrity** -- on create/update, verifies referenced targets exist. On delete, enforces `RESTRICT`, `CASCADE`, or `SET_NULL` by querying covering indexes.

### index/

Secondary index derivation, storage, and merge.

- **IndexDerivation** -- extracts key/order values from a protobuf payload and produces `DerivedIndexEntry` records, including repeated-field expansion.
- **IndexSchemaGenerator** -- generates synthetic protobuf message descriptors for index key and value types.
- **IndexObject** -- reads/writes index entry objects (protobuf-serialized sorted row sets).
- **IndexMerge** -- three-way merges of index entries on transaction commit.
- **IndexConflictResolver** -- detects unique-index violations and classifies index-level conflicts.
- **IndexUtils** -- helpers for adding/removing individual rows from index objects.

### registry/

Type registration and schema evolution.

- **TypeRegistry** -- full `RegisterTypeVersion` workflow: compile the proto, check schema compatibility, create TypeDefinition/TypeVersionDefinition/IndexDefinition/ReferenceDefinition artifacts atomically.
- **ProtoCompiler** -- runtime `.proto` compilation via the protobuf `Parser` API (thread-safe, no `protoc` binary needed).
- **SchemaCompatibility** -- validates that a new type version is additive-only (no removed fields, no changed types, no removed indexes).

### encoding/

Binary encoding for index keys and storage paths.

- **IndexKeyEncoder** -- variable-length integer and composite key encoding for index lookups.
- **Base64Url** -- URL-safe base64 for storage object paths.
- **ArtifactPath** -- constructs `artifacts/{id}` and `indexes/{def_id}/{encoded_key}` paths.

### bootstrap/

Genesis initialization. `RunGenesis` creates all built-in type and index artifacts (IDs 1--20) in a single atomic commit. Idempotent -- if genesis state already exists, it returns the existing index mapping.

### id/

Client for an external ID allocator service. Tests use a `MockIdAllocator` that hands out sequential IDs.

## Built-in Types

The system bootstraps five built-in artifact types at genesis. User-allocated artifact IDs start at 21.

| Type | Defined in | Purpose |
|------|-----------|---------|
| `IndexDefinition` | `artifact_options.proto` | Declares a secondary index. Also the protobuf option message used in `option (indexes) = { ... }`. |
| `TypeDefinition` | `artifact_types.proto` | Logical artifact type. Tracks the type name, current version, and mutation-deny flags. |
| `TypeVersionDefinition` | `artifact_types.proto` | A specific schema version. Carries the compiled `FileDescriptorSet` and original `.proto` source. |
| `ReferenceDefinition` | `artifact_types.proto` | A foreign-key relationship. Records the target type, source type, field path, covering index, and `on_delete` action. |
| `TransactionCommitRecord` | `artifact_types.proto` | Committed transaction record for double-commit prevention. |

`StoredArtifact` in `artifact_internal.proto` is the on-disk envelope wrapping every artifact, but it is not a registered type -- it is an internal storage detail.

## Proto Files

| File | Contents |
|------|----------|
| `proto/artifact_options.proto` | Custom protobuf extensions (`indexes`, `references`, `message_description`, `field_description`) and the `IndexDefinition` / `OrderDefinition` / `ReferenceOption` messages |
| `proto/artifact_types.proto` | Built-in type payloads: `TypeDefinition`, `TypeVersionDefinition`, `ReferenceDefinition`, `TransactionCommitRecord` |
| `proto/artifact_internal.proto` | `StoredArtifact` on-disk envelope (not public API) |
| `proto/artifact_service.proto` | gRPC service definitions, request/response messages, and error types |

## Building and Testing

The build uses CMake with Ninja and requires a C++23 compiler. The first build fetches dependencies (gRPC, protobuf, googletest, nlohmann/json) and compiles everything, which is slow. Subsequent builds are incremental -- only changed files are recompiled.

### Incremental build and test (recommended)

If the build directory already exists (`build/release/`), skip the configure step. Only run `cmake --preset` on first setup or after CMakeLists.txt changes.

```bash
# First time only (or after CMakeLists.txt changes):
cmake --preset release

# Iterative development -- only recompiles changed files:
cmake --build --preset release
ctest --preset release
```

### Running specific tests

CTest filters use `-R` (regex match) against the full test name, which is `SuiteName.TestName` -- not the binary name. Use `-E` to exclude.

```bash
ctest --preset release -R IndexDerivationTest         # all tests in one suite
ctest --preset release -R "^Index"                    # all Index* suites
ctest --preset release -E "LakeFS"                    # skip LakeFS integration tests
ctest --preset release -V                             # verbose output
```

### Building a single target

```bash
cmake --build --preset release --target artifact_layer_lib       # core library only
cmake --build --preset release --target artifact_layer_service   # server executable only
cmake --build --preset release --target index_derivation_test    # single test binary only
```

### Build presets

| Preset | Flags | Use case |
|--------|-------|----------|
| `release` | `-O2 -DNDEBUG` | Default. Fast tests, CI. |
| `debug` | `-g -O0` | Debugging with full symbols. |
| `asan` | `-fsanitize=address,undefined` | Memory error and UB detection. |
| `tsan` | `-fsanitize=thread` | Data race detection. |

### Docker / Makefile

When Docker is available, the `Makefile` wraps builds inside the `cpp23-toolchain` image:

```bash
make build                  # configure + build (default preset: release)
make test                   # configure + build + run all tests
make build PRESET=asan      # build with AddressSanitizer
```

Note: each `make` invocation starts a fresh container, so it re-runs configure every time. For iterative development, start a persistent shell instead:

```bash
docker run -it --rm \
  --mount type=bind,source=$(pwd)/..,target=/code/artifact-system \
  --mount type=bind,source=$(pwd)/../../id-allocator,target=/code/id-allocator \
  -w /code/artifact-system/artifact-layer \
  cpp23-toolchain
```

Then use the `cmake --build` / `ctest` commands directly inside the container.

### Local LakeFS (for integration tests)

```bash
make lakefs-up          # start local LakeFS
make lakefs-bootstrap   # initialize repository
make lakefs-status      # check status
make lakefs-down        # tear down
```

## Running the server

`artifact_layer_service` is configured entirely through environment variables:

| Variable | Meaning | Default |
|----------|---------|---------|
| `ARTIFACT_LAYER_LISTEN_ADDRESS` | gRPC listen address | `0.0.0.0:50051` |
| `ARTIFACT_LAYER_STORAGE_TYPE` | Storage backend: `memory` or `lakefs` | `memory` |
| `LAKEFS_ENDPOINT` | LakeFS API endpoint URL | required for `lakefs` |
| `LAKEFS_ACCESS_KEY_ID` | LakeFS access key ID | required for `lakefs` |
| `LAKEFS_SECRET_ACCESS_KEY` | LakeFS secret access key (injected from Secret Manager in production) | required for `lakefs` |
| `LAKEFS_REPOSITORY` | LakeFS repository name | required for `lakefs` |
| `LAKEFS_CANONICAL_BRANCH` | Canonical branch name | `main` |
| `ARTIFACT_LAYER_ID_ALLOCATOR_ADDRESS` | id-allocator service address | mock allocator when unset |
| `ARTIFACT_LAYER_ID_ALLOCATOR_PARTITION_ID` | id-allocator partition ID | required with the address |
| `ARTIFACT_LAYER_ID_ALLOCATOR_HIGH_WATER_MARK` | Pre-allocation refill threshold | `1000` |

`lakefs` storage requires the production ID allocator: persistent storage with
the mock allocator would re-issue IDs already present in storage after a
restart. `memory` storage may be combined with either allocator.

Genesis runs at startup against the configured backend and is idempotent, so
restarting against an already-initialized LakeFS repository is safe. The
canonical branch must already exist in the repository — genesis initializes
its contents but does not create the branch.

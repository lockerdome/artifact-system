# Artifact System Implementation Plan

This document breaks the Artifact System PRD into sequenced, buildable phases. Each
phase produces a compilable + testable artifact. Later phases depend on earlier ones.

The implementation lives at `artifact-system/artifact-layer/` inside the monorepo. The parent
`artifact-system/` directory is the umbrella for the full system (PRD, gcloud provisioning,
and eventually app-layer services). This matches the PRD's layered terminology: Storage Layer,
Artifact Layer, App Layer.

The build follows the same conventions as `id-allocator/id-allocator-service/` and
`id-allocator/id-allocator-client-cpp/` (CMake + Ninja + C++23, FetchContent for deps,
GoogleTest, CMakePresets, Makefile for Docker-based builds, GitHub Actions CI).

All file paths in phases below are relative to `artifact-system/artifact-layer/` unless
otherwise noted.

---

## Progress Tracker

| Phase | Description | Status |
|-------|-------------|--------|
| P0 | Project Scaffolding & Build Infrastructure | :white_check_mark: Complete |
| P1 | Proto Definitions (System + API) | :white_check_mark: Complete |
| P2a | Storage Layer Abstraction & In-Memory Implementation | :white_check_mark: Complete |
| P2b | LakeFS Storage Layer Integration | :white_check_mark: Complete |
| P3 | Path Encoding & Index Key Encoding | :white_check_mark: Complete |
| P4 | Index Storage & Three-Way Merge | :white_check_mark: Complete |
| P5 | Transaction & Snapshot Manager | :white_check_mark: Complete |
| P6 | Artifact CRUD Engine | :construction: In progress |
| P7a | Registry Utilities (proto compiler, schema compat) | :white_large_square: Not started |
| P7b | Type Registry Orchestration (RegisterTypeVersion + introspection) | :white_large_square: Not started |
| P8 | Genesis Bootstrap | :white_large_square: Not started |
| P9 | gRPC Server & Service Implementation | :white_large_square: Not started |
| P10 | ID Allocator Production Integration | :white_large_square: Not started |

**Status legend**: :white_large_square: Not started | :construction: In progress | :white_check_mark: Complete

---

## Phase 0 — Project Scaffolding & Build Infrastructure

**Goal**: Empty library + test binary compiles and CI passes.

### Deliverables
1. **Directory structure**
   ```
   artifact-system/
     artifact-layer/
       CMakeLists.txt
       CMakePresets.json        # debug / release / asan / tsan (mirror id-allocator-service)
       Makefile                 # toolchain / configure / build / test / run targets
       .gitignore
       cmake/
         dependencies.cmake     # FetchContent: gRPC (brings protobuf), GoogleTest,
                                #   id-allocator-client-cpp (local path)
       proto/
         artifact_system.proto  # system protos (IndexDefinition, ReferenceOption, etc.)
         artifact_service.proto # ArtifactService, IndexService, SnapshotTransactionService,
                                #   TypeRegistryService
       src/
         main.cpp               # placeholder server entry point
       tests/
         CMakeLists.txt
         placeholder_test.cpp   # single GoogleTest that passes (proves build works)
   ```

2. **GitHub Action**: `.github/workflows/artifact_layer_tests.yml`
   - Triggers on `artifact-system/artifact-layer/**`, workflow file changes
   - `working-directory: artifact-system/artifact-layer`
   - Same pattern as `id_allocator_service_tests.yml` (ubuntu-24.04, gcc-14, cmake presets,
     cache `_deps`)

3. **Proto compilation**: CMake custom commands to generate C++ stubs from
   `proto/artifact_system.proto` and `proto/artifact_service.proto`
   - Include paths for protobuf well-known types and id-allocator proto (for ID client)
   - gRPC plugin for service stubs

4. **Dependency on id-allocator-client-cpp**: The CMake build should be able to consume the
   `id_allocator_client_cpp_lib` target. Options:
   - `add_subdirectory(../../../id-allocator/id-allocator-client-cpp ...)` with relative path
   - Or FetchContent from local path
   - The dependency provides `IdAllocatorClient` for allocating artifact IDs

5. **`src/id/id_allocator_interface.h`** — abstract ID allocator interface + mock:
   - `IdAllocatorInterface` with a single `virtual uint64_t AllocateId() = 0`
   - Throws `std::runtime_error` on failure — ID exhaustion is treated as a catastrophic
     error (the gRPC service layer translates exceptions to `UNAVAILABLE` status)
   - `MockIdAllocator` — returns sequential IDs starting from a configurable base
    - This is trivial scaffolding but must exist early so that P6 (CRUD) and P8 (genesis)
      can compile and test without the real ID service. The production wrapper around
      `id_allocator::client::IdAllocatorClient` is deferred to P10.

### Checklist
- [x] Directory structure created
- [x] `CMakeLists.txt`, `CMakePresets.json`, `Makefile`, `.gitignore`
- [x] `cmake/dependencies.cmake` (gRPC, protobuf, GoogleTest, id-allocator-client-cpp)
- [x] Placeholder proto files compile
- [x] Placeholder `src/main.cpp`
- [x] `tests/placeholder_test.cpp` passes
- [x] `.github/workflows/artifact_layer_tests.yml` created
- [x] `src/id/id_allocator_interface.h` — abstract interface + `MockIdAllocator`
- [x] `cmake --preset release && cmake --build --preset release && ctest --preset release` succeeds

---

## Phase 1 — Proto Definitions (System + API)

**Goal**: Complete proto definitions matching the PRD, compilable, with generated C++ stubs.

### Deliverables

1. **`proto/artifact_options.proto`** — custom option extensions:
   - `IndexDefinition` message (key_type, key, order, unique, optional WhereClause post-MVP)
   - `OrderDefinition` message (field, direction enum)
   - `ReferenceOption` message (target_type_name, on_delete enum)
   - `extend google.protobuf.MessageOptions` — `repeated IndexDefinition indexes = 50002`,
     `optional string description = 50004`
   - `extend google.protobuf.FieldOptions` — `optional ReferenceOption references = 50003`,
     `optional string description = 50005`

2. **`proto/artifact_types.proto`** — built-in type messages:
   - `TypeDefinition` (type_name, current_version_id, deny_create/update/delete)
   - `TypeVersionDefinition` (type_id, descriptor_set, proto_source,
     previous_version_id, next_version_id)
   - `IndexDefinition` (as artifact payload — key_type, key, order, unique)
   - `ReferenceDefinition` (key_type, target_type_name, referencing_type_name,
     field_name, covering_index_key_type, on_delete)
   - Each with `option (indexes) = { ... }` from the PRD

3. **`proto/artifact_service.proto`** — gRPC service definitions:
   - `SnapshotTransactionService` (CreateSnapshot, CreateTransaction, CommitTransaction,
     RollbackTransaction)
   - `ArtifactService` (CreateArtifact, GetArtifact, BatchGetArtifacts, UpdateArtifact,
     DeleteArtifact)
   - `IndexService` (FetchIndex)
   - `TypeRegistryService` (RegisterTypeVersion, GetTypeVersion, ListTypeVersions,
     GetIndexSchema)
   - All request/response messages, `ReadContext`, error detail messages
     (`CommitConflict`, `ArtifactWriteError`, `ArtifactWriteViolation`,
     `SnapshotTransactionError`, `FetchIndexError`, `RegisterTypeVersionError`,
     `TypeRegistrationViolation`, `ArtifactNotFoundError`)

4. **`proto/artifact_internal.proto`** — internal storage format (not part of the public API):
   - `StoredArtifact` envelope: `{ uint32 envelope_version, uint64 version_id,
     string type_name, bytes payload }`. This is the on-disk format at `artifacts/{id}`.
     Storing `type_name` alongside `version_id` avoids cascading reads on every GetArtifact.

5. Update `CMakeLists.txt` to compile all protos and produce an `artifact_layer_proto` library
   target.

### Checklist
- [x] `proto/artifact_options.proto` — custom option extensions
- [x] `proto/artifact_types.proto` — built-in type messages with index options
- [x] `proto/artifact_service.proto` — all 4 gRPC services + all request/response/error messages
- [x] `proto/artifact_internal.proto` — StoredArtifact envelope
- [x] `artifact_layer_proto` CMake library target compiles
- [x] Generated C++ headers usable from test code
- [x] All fields, enums, services, and error messages match the PRD

---

## Phase 2a — Storage Layer Abstraction & In-Memory Implementation

**Goal**: Define C++ interfaces for the Storage Layer and provide an in-memory implementation
so the Artifact Layer can be built and tested without LakeFS.

### Deliverables

1. **`src/storage/storage_interface.h`** — abstract interface:
   ```cpp
   class StorageInterface {
   public:
     // Branch operations
     virtual StatusOr<BranchId> CreateBranch(string name, CommitId base) = 0;
     virtual Status DeleteBranch(BranchId) = 0;
     virtual StatusOr<CommitId> GetBranchHead(BranchId) = 0;

     // Object I/O (within a branch's staging area)
     virtual Status PutObject(BranchId, string path, bytes data) = 0;
     virtual StatusOr<bytes> GetObject(BranchId, string path) = 0;
     virtual StatusOr<bytes> GetObjectAtCommit(CommitId, string path) = 0;
     virtual Status DeleteObject(BranchId, string path) = 0;
     virtual bool ObjectExists(BranchId, string path) = 0;

     // Commit and merge
     virtual StatusOr<CommitId> Commit(BranchId, string message) = 0;
     virtual StatusOr<MergeResult> Merge(BranchId source, BranchId target) = 0;
     // MergeResult: success + CommitId | conflicts list

     // Canonical branch
     virtual BranchId GetCanonicalBranch() = 0;
   };
   ```

2. **`src/storage/memory_storage.h/.cpp`** — in-memory implementation:
   - Branch = name -> ordered map of `path -> (data, committed?)` + commit history
   - Commit = snapshot of all objects at that point
   - Merge = three-way diff (base, source head, target head) -> detect conflicts
   - Good enough for unit testing all Artifact Layer logic

3. **Shared conformance test suite**: `tests/storage_conformance_test.h`
   - Parameterized GoogleTest suite that exercises `StorageInterface` through its abstract API
   - Both `MemoryStorage` (this phase) and `LakeFSStorage` (P2b) register as
     implementations so the same test logic validates both backends

4. **Tests**: `tests/memory_storage_test.cpp`
   - Instantiates the conformance suite against `MemoryStorage`
   - Branch CRUD, put/get/delete objects, commit, merge, conflict detection

### Checklist
- [ ] `src/storage/storage_interface.h` — abstract interface defined
- [ ] `src/storage/memory_storage.h/.cpp` — in-memory implementation
- [ ] `tests/storage_conformance_test.h` — parameterized conformance suite
- [ ] `tests/memory_storage_test.cpp` — conformance suite passes for MemoryStorage
- [ ] Interface covers: branch CRUD, object I/O, commit, merge, conflict detection, canonical branch

---

## Phase 2b — LakeFS Storage Layer Integration (parallel with P2a)

**Goal**: Implement `StorageInterface` against a real LakeFS instance. This runs in parallel
with P2a to de-risk the highest-uncertainty dependency early. The interface defined in P2a is
the contract; P2b validates that LakeFS actually conforms to it.

### Risk areas to validate early
- Does LakeFS's branch/merge/conflict API map cleanly to our interface?
- What does the conflict response actually look like? (path-level conflicts, merge base
  availability)
- Latency characteristics of branch creation, commit, and merge operations
- Object path length limits in practice (GCS 1024-byte limit via LakeFS)
- Any semantic surprises with three-way merge conflict detection (e.g., does LakeFS provide
  the merge base commit, or do we need to compute it?)
- S3 gateway vs. REST API trade-offs for object I/O

### Deliverables

1. **`src/storage/lakefs_storage.h/.cpp`**:
   - LakeFS API client for branch, object, commit, and merge operations
   - Decide on API surface: LakeFS REST API vs. S3-compatible gateway (or a mix — REST for
     branch/commit/merge, S3 for object I/O)
   - Map `StorageInterface` methods to LakeFS API calls
   - Handle LakeFS-specific merge conflict response format and translate to
     `MergeResult::Conflict` with object paths
   - Configuration (endpoint, credentials from Secret Manager)

2. **`src/storage/lakefs_config.h`**:
   - Config struct: endpoint URL, access key, secret key, repo name, canonical branch name

3. **Shared conformance test suite**: `tests/storage_conformance_test.h`
   - A parameterized GoogleTest suite that exercises `StorageInterface` through its abstract
      API. Both `MemoryStorage` (P2a) and `LakeFSStorage` (P2b) register as implementations
     so the same test logic validates both backends.
   - Covers: branch CRUD, object put/get/delete, commit, merge, conflict detection

4. **Tests**: `tests/lakefs_storage_test.cpp`
   - Instantiates the conformance suite against a LakeFS instance
   - Requires a running LakeFS (local Docker or test environment); can be marked as an
     integration test that CI skips by default but runs on demand
   - Additional LakeFS-specific tests: connection handling, auth, error mapping

### Checklist
- [x] `src/storage/lakefs_storage.h/.cpp` — LakeFS API client implementing StorageInterface
- [x] `src/storage/lakefs_config.h` — configuration struct
- [x] `tests/lakefs_storage_test.cpp` — conformance suite passes against real LakeFS
- [x] Conflict response correctly translated to `MergeResult`
- [x] Merge base accessible for three-way diff
- [x] Risk findings documented in `lakefs_storage.h` header comment

---

## Phase 3 — Path Encoding & Index Key Encoding

**Goal**: Implement the deterministic encoding and path generation schemes from the PRD.

### Deliverables

1. **`src/encoding/base64url.h/.cpp`** — URL-safe base64 without padding
   - `std::string Encode(std::span<const uint8_t> bytes)`
   - Used for artifact_id path segments (big-endian uint64 -> 11-char base64url)
   - Used for index key_prefix (same encoding)
   - Used for index key_hash (SHA-256 digest -> 43-char base64url)

2. **`src/encoding/artifact_path.h/.cpp`** — path helpers:
   - `ArtifactPath(uint64_t artifact_id)` -> `"artifacts/{base64url(BE(artifact_id))}"`
   - `IndexPath(uint64_t index_def_id, span<uint8_t> encoded_key)` ->
     `"indexes/{base64url(BE(index_def_id))}/{base64url(SHA256(encoded_key))}"`

3. **`src/encoding/index_key_encoder.h/.cpp`** — deterministic binary index key encoding:
   - Per-type encoders matching the PRD key encoding rules:
     - int32/sint32/sfixed32: 4-byte LE two's complement
     - uint32/fixed32: 4-byte LE unsigned
     - int64/sint64/sfixed64: 8-byte LE two's complement
     - uint64/fixed64: 8-byte LE unsigned
     - bool: 1 byte
     - enum: 4-byte signed LE
     - float: IEEE 754 binary32 LE (reject NaN, normalize -0)
     - double: IEEE 754 binary64 LE (reject NaN, normalize -0)
     - string: varint-length-prefixed UTF-8 bytes
     - bytes: varint-length-prefixed raw bytes
   - Varint encoder (minimal encoding, reject non-minimal)
   - `EncodeKey(descriptor, message, key_fields)` -> `vector<uint8_t>`

4. **Tests**: `tests/base64url_test.cpp`, `tests/artifact_path_test.cpp`,
   `tests/index_key_encoder_test.cpp`
   - Known-answer vectors for each encoding type
   - Round-trip tests
   - NaN rejection, -0 normalization
   - Non-minimal varint rejection
   - Path length assertions (63 bytes for index paths)

### Checklist
- [x] `src/encoding/base64url.h/.cpp` — URL-safe base64 without padding
- [x] `src/encoding/artifact_path.h/.cpp` — artifact and index path generation
- [x] `src/encoding/index_key_encoder.h/.cpp` — all PRD key encoding rules
- [x] `tests/base64url_test.cpp` — known-answer vectors
- [x] `tests/artifact_path_test.cpp` — path format and length assertions
- [x] `tests/index_key_encoder_test.cpp` — all types, NaN rejection, -0 normalization, varint validation

---

## Phase 4 — Index Storage & Three-Way Merge

**Goal**: Implement index object read/write, the three-way merge algorithm, and index schema
generation. Index schema generation (building `IndexKey_*`/`IndexValue_*`/`Index_*` message
descriptors) lives here because index serialization/deserialization depends on these schemas.

### Deliverables

1. **`src/index/index_schema_generator.h/.cpp`**:
   - Generate `IndexKey_*`, `IndexValue_*`, `Index_*` message descriptors from an
     IndexDefinition + parent type descriptor
   - Builds `FileDescriptorProto` programmatically using the protobuf descriptor API
     (no dependency on libprotoc)
    - Used by index_object for serialization/deserialization at runtime, and by P7b's
     GetIndexSchema for API responses

2. **`src/index/index_object.h/.cpp`** — index object serialization/deserialization:
   - Serialize/deserialize index objects using protobuf deterministic serialization
   - Validate row_count matches column lengths
   - Uses schemas from `index_schema_generator` for `Index_*`, `IndexKey_*`, `IndexValue_*`
   - Work with `google::protobuf::DynamicMessage` for runtime schema handling

3. **`src/index/index_merge.h/.cpp`** — three-way index merge:
   - Input: base index object, "ours" index object, "theirs" index object
   - Compute adds (head minus base) and removes (base minus head) per branch
   - Apply removes to base, apply adds, de-duplicate by artifact_id, re-sort by order fields
   - For unique indexes: if result has >1 entry, return conflict
   - Deterministic and idempotent (same inputs -> same output)

4. **`src/index/index_derivation.h/.cpp`** — derive index entries from an artifact:
   - Given a descriptor set with index custom options and a serialized artifact payload,
     extract all index entries that should exist for this artifact
   - Handle repeated fields (one entry per element)
   - Reject NaN in indexed float fields, non-minimal varints
   - Return list of `(index_def_id, encoded_key, order_field_values)` tuples

5. **Tests**: `tests/index_schema_generator_test.cpp`, `tests/index_object_test.cpp`,
   `tests/index_merge_test.cpp`, `tests/index_derivation_test.cpp`
   - Schema generation: correct IndexKey_*/IndexValue_*/Index_* layout
   - Merge: no-conflict, add+add (non-unique), add+remove, remove+remove,
     unique index conflict
   - Derivation: simple fields, repeated fields, optional fields (missing = no entry),
     multiple indexes per type
   - Edge cases: empty indexes, tombstoned indexes (row_count=0)

### Checklist
- [x] `src/index/index_schema_generator.h/.cpp` — generate IndexKey_*/IndexValue_*/Index_* descriptors
- [x] `tests/index_schema_generator_test.cpp` — generated schemas match expected layout
- [x] `src/index/index_object.h/.cpp` — serialize/deserialize with DynamicMessage, row_count validation
- [x] `src/index/index_merge.h/.cpp` — three-way merge (deterministic, idempotent)
- [x] `src/index/index_derivation.h/.cpp` — derive entries from artifact payload + descriptor set
- [x] `tests/index_object_test.cpp`
- [x] `tests/index_merge_test.cpp` — no-conflict, add+add, add+remove, unique conflict
- [x] `tests/index_derivation_test.cpp` — all field types, repeated fields, optional presence, NaN rejection

---

## Phase 5 — Transaction & Snapshot Manager

**Goal**: Implement the transaction/snapshot lifecycle on top of the Storage Layer abstraction.

### Deliverables

1. **`src/transaction/transaction_manager.h/.cpp`**:
   - `CreateSnapshot(optional<parent_id>)` -> snapshot_id (commit pointer)
   - `CreateTransaction(optional<parent_id>)` -> transaction_id (ephemeral branch)
   - `CommitTransaction(transaction_id)` -> merge into parent/canonical branch
   - `RollbackTransaction(transaction_id)` -> delete ephemeral branch
   - Internal ID allocation for snapshot/transaction IDs (can use the ID allocator client
     or a local counter for these internal IDs)
   - Nested transactions (sub-transaction merges into parent's branch)
   - Implicit transaction support (create branch, do work, commit, merge, return)

2. **`src/transaction/write_executor.h/.cpp`**:
   - Sub-branch-per-write pattern: fork child branch from transaction head, stage artifact
     + index updates, commit, merge child into transaction branch
   - Conflict retry logic: if merge fails with index conflicts, read conflicting objects,
     run three-way merge, re-stage, retry (up to 5 attempts, exponential backoff with jitter)
   - Distinguish retryable (non-unique index) vs non-retryable (unique index, payload) conflicts

3. **`src/transaction/conflict_resolver.h/.cpp`**:
   - Implements the Conflict retry policy from the PRD
   - Classify conflicts by type (index vs payload)
   - For eligible index conflicts, invoke index merge and retry
   - Build `CommitConflict` response when retries exhausted or non-retryable

4. **Tests**: `tests/transaction_manager_test.cpp`, `tests/write_executor_test.cpp`,
   `tests/conflict_resolver_test.cpp`
   - Snapshot creation, transaction lifecycle (create/commit/rollback)
   - Nested transactions
   - Sub-branch-per-write isolation
   - Conflict retry: success after retry, exhaustion, non-retryable immediate failure
   - All tests use `MemoryStorage`

### Checklist
- [x] `src/transaction/transaction_manager.h/.cpp` — snapshot/transaction lifecycle
- [x] `src/transaction/write_executor.h/.cpp` — sub-branch-per-write, conflict retry
- [x] `src/transaction/conflict_resolver.h/.cpp` — classify conflicts, retry policy, CommitConflict
- [x] `tests/transaction_manager_test.cpp` — create/commit/rollback, nested transactions
- [x] `tests/write_executor_test.cpp` — sub-branch isolation, concurrent writes
- [x] `tests/conflict_resolver_test.cpp` — retry success, exhaustion, non-retryable conflicts
- [x] Implicit transaction support working
- [ ] TSan preset passing (see note below)

### TSan note

The TSan (ThreadSanitizer) preset does not currently work under GCC with gRPC v1.78.0.
TSan-instrumented protoc crashes at startup ("unexpected memory mapping"), and building
all dependencies with TSan triggers abseil constexpr failures. P5 is the first phase that
introduces real concurrency (conflict retry, concurrent writes), making TSan coverage
important. Before starting P5, either:

1. Switch the toolchain to Clang (fixes both TSan and the abseil constexpr issue), or
2. Upgrade gRPC/abseil to a version that compiles cleanly under GCC + TSan.

The ASan+UBSan preset works (sanitizer flags are stripped from dependencies via
`cmake/dependencies.cmake`). The same approach cannot work for TSan because TSan
requires all linked code in a process to be instrumented.

---

## Phase 6 — Artifact CRUD Engine

**Goal**: Core artifact create/read/update/delete logic with validation.

**Note on type resolution**: P6 resolves TypeVersionDefinition and TypeDefinition by reading
them as regular artifacts (by artifact_id via storage). It does not depend on P7b's registry
orchestration. The resolution path is: caller provides version_id → read
TypeVersionDefinition artifact → extract type_id → read TypeDefinition artifact → extract
type_name and deny_* flags. This is plain artifact reads, not registry API calls.

### Deliverables

1. **`src/artifact/artifact_store.h/.cpp`**:
   - `CreateArtifact(version_id, payload, optional<transaction_id>)`:
     - Allocate ID via `IdAllocatorClient`
     - Resolve TypeVersionDefinition -> TypeDefinition (direct artifact reads)
     - Validate mutation restrictions (deny_create)
     - Validate payload (empty check, proto3 structural)
     - Derive index entries
     - Stage artifact + index updates via WriteExecutor
   - `GetArtifact(artifact_id, ReadContext)`:
     - Read from snapshot/transaction/canonical branch
     - Return payload + type_name + version_id
     - Tombstone = not found
   - `BatchGetArtifacts(artifact_ids, ReadContext)`:
     - Per-id results preserving positional correlation
   - `UpdateArtifact(artifact_id, version_id, payload, optional<transaction_id>)`:
     - Read existing artifact, validate type_name match
     - Validate mutation restrictions (deny_update)
     - Validate payload, derive new index entries
     - Compute index diff (remove old entries, add new entries)
     - Stage via WriteExecutor
   - `DeleteArtifact(artifact_id, optional<transaction_id>)`:
     - Validate mutation restrictions (deny_delete)
     - Write tombstone (empty payload)
     - Remove all derived index entries
     - Referential integrity enforcement (RESTRICT/CASCADE/SET_NULL)

2. **`src/artifact/validation.h/.cpp`**:
   - Validation pipeline matching the PRD order:
     1. version_id resolution (short-circuit)
     2. Mutation restriction check (short-circuit)
     3. Empty payload check (short-circuit)
     4. Proto3 structural validation (short-circuit)
     5. Index derivation checks (NaN, non-minimal varint) — collect all
     6. Referential integrity checks — collect all
   - Build `ArtifactWriteError` with collected violations

3. **`src/artifact/referential_integrity.h/.cpp`**:
   - Write-time validation (target exists, not tombstoned, correct type, no duplicates
     in repeated)
   - Delete-time enforcement (RESTRICT/CASCADE/SET_NULL)
   - Cascade cycle detection (track scheduled deletes)

4. **Tests**: `tests/artifact_store_test.cpp`, `tests/validation_test.cpp`,
   `tests/referential_integrity_test.cpp`
   - CRUD operations against in-memory storage
   - All violation categories tested
   - Validation ordering and short-circuit behavior
   - Referential integrity: all on_delete behaviors, cascading cycles

### Checklist
- [ ] `src/artifact/artifact_store.h/.cpp` — Create/Get/BatchGet/Update/Delete
- [ ] `src/artifact/validation.h/.cpp` — full validation pipeline with PRD ordering
- [ ] `src/artifact/referential_integrity.h/.cpp` — write-time + delete-time enforcement
- [ ] `tests/artifact_store_test.cpp` — CRUD with implicit and explicit transactions
- [ ] `tests/validation_test.cpp` — all 11 violation categories, short-circuit behavior
- [ ] `tests/referential_integrity_test.cpp` — RESTRICT, CASCADE, SET_NULL, cycle detection
- [ ] ID allocation via mock allocator (real integration in P10)

---

## Phase 7a — Registry Utilities

**Goal**: Build the self-contained utility modules that RegisterTypeVersion depends on.
These are independently testable with no dependency on the artifact CRUD engine.
Note: `index_schema_generator` lives in P4 (where index serialization needs it);
P7b's GetIndexSchema reuses it from there.

### Deliverables

1. **`src/registry/proto_compiler.h/.cpp`**:
   - Runtime .proto compilation into FileDescriptorSet
   - System proto injection (artifact_options.proto as well-known import via custom
     `SourceTree` that provides exactly system protos + the user's submitted source)
   - Resource limits (max file size, compilation timeout, nesting depth)
   - Error reporting as PROTO_COMPILATION_FAILURE
   - Thread safety: serialize all compilations behind a single-threaded executor (libprotoc
     is not thread-safe). Use a fresh `DescriptorPool` and `Importer` per compilation to
     avoid accumulated memory.

2. **`src/registry/schema_compatibility.h/.cpp`**:
   - Compare two FileDescriptorSets for compatibility
   - Detect removed fields, type changes (including between optional/required/repeated),
     oneof changes (field removed from oneof, oneof itself removed), field number
     reassignment
   - Recursive nested message validation (PRD: "rules apply recursively to nested message
     types")
   - Report SCHEMA_INCOMPATIBILITY violations

3. **Tests**: `tests/proto_compiler_test.cpp`, `tests/schema_compatibility_test.cpp`
   - Proto compilation with system imports, error cases, resource limit violations
   - All SCHEMA_INCOMPATIBILITY cases (field removal, type change, oneof changes, field
     number reassignment, nested message changes at 3+ depth levels)

### Checklist
- [ ] `src/registry/proto_compiler.h/.cpp` — runtime .proto compilation with system proto injection
- [ ] `src/registry/schema_compatibility.h/.cpp` — field/type/oneof/nested change detection
- [ ] `tests/proto_compiler_test.cpp` — compilation, system imports, resource limits, thread safety
- [ ] `tests/schema_compatibility_test.cpp` — all SCHEMA_INCOMPATIBILITY cases including nested messages
- [ ] `libprotoc` confirmed available as CMake target and linked

---

## Phase 7b — Type Registry Orchestration

**Goal**: Wire the P7a utilities into the full RegisterTypeVersion workflow and type
introspection APIs. This is the most complex orchestration in the system: a single
RegisterTypeVersion call creates/updates multiple artifact types in a single transaction
with internal bypass of mutation restrictions.

### Deliverables

1. **`src/registry/type_registry.h/.cpp`**:
   - `RegisterTypeVersion(type_name, proto_source, deny_flags)`:
      - Compile .proto source via `ProtoCompiler` (P7a)
     - Locate message by type_name in descriptor set
     - Look up / create TypeDefinition (via type_name_unique index)
     - Tighten-only enforcement for mutation flags (TIGHTEN_ONLY_VIOLATION)
      - Schema compatibility validation via `SchemaCompatibility` (P7a)
     - Extract + validate index definitions, create IndexDefinition artifacts
       (INVALID_INDEX_DEFINITION, INDEX_INCOMPATIBILITY)
     - Extract + validate reference declarations, create ReferenceDefinition artifacts
       (INVALID_REFERENCE_DECLARATION, REFERENCE_INCOMPATIBILITY)
     - Create TypeVersionDefinition, link into doubly-linked list (update tail's
       next_version_id via internal bypass of deny_update)
     - Update TypeDefinition.current_version_id (via internal bypass of deny_update)
     - All in a single Artifact Layer transaction
   - `GetTypeVersion(version_id)` -> TypeVersionDefinition details
   - `ListTypeVersions(type_name)` -> version IDs via type_versions_by_type index
   - `GetIndexSchema(key_type)` -> IndexDefinition + generated index proto schema
      (using `IndexSchemaGenerator` from P4)

2. **`src/registry/internal_bypass.h`**:
   - Mechanism for internal artifact layer operations to bypass mutation restriction checks
     (deny_create, deny_update, deny_delete) on built-in types
   - Scoped/token-based so it cannot leak to external callers

3. **Tests**: `tests/type_registry_test.cpp`
   - Register new type (creates TypeDefinition + TypeVersionDefinition + IndexDefinitions +
     ReferenceDefinitions atomically)
   - Register new version (schema compat check, doubly-linked list linkage,
     current_version_id update)
   - Tighten-only flags (false->true allowed, true->false rejected)
   - Concurrent registration for same type produces payload conflict
   - All 7 RegisterTypeVersion violation categories tested
   - GetTypeVersion, ListTypeVersions, GetIndexSchema introspection

### Checklist
- [ ] `src/registry/type_registry.h/.cpp` — RegisterTypeVersion orchestration
- [ ] `src/registry/internal_bypass.h` — scoped mutation restriction bypass
- [ ] `tests/type_registry_test.cpp` — register, version, tighten-only, concurrent conflict
- [ ] GetTypeVersion, ListTypeVersions, GetIndexSchema working
- [ ] All 7 RegisterTypeVersion violation categories tested
- [ ] Internal bypass limited to system-managed operations only

---

## Phase 8 — Genesis Bootstrap

**Goal**: Initialize the system with all built-in types, indexes, and their derived entries.

### Deliverables

1. **`src/bootstrap/genesis.h/.cpp`**:
   - Create all built-in artifacts in the PRD-specified dependency order:
     1. IndexDefinition type (TypeDefinition + TypeVersionDefinition)
     2. `index_key_type_unique` IndexDefinition artifact
     3. `all_index_definitions` IndexDefinition artifact
     4. TypeDefinition type (TypeDefinition + TypeVersionDefinition)
     5. `type_name_unique`, `all_types` IndexDefinition artifacts
     6. TypeVersionDefinition type (TypeDefinition + TypeVersionDefinition)
     7. `type_versions_by_type` IndexDefinition artifact
     8. ReferenceDefinition type (TypeDefinition + TypeVersionDefinition)
     9. `reference_key_type_unique`, `references_by_target_type`,
        `all_reference_definitions` IndexDefinition artifacts
     10. Built-in ReferenceDefinition artifacts
     11. All derived index entries
   - Pre-allocated IDs (no ID service dependency at genesis)
   - Single atomic commit to canonical branch
   - Idempotent: no-op if genesis state already exists

2. **Tests**: `tests/genesis_test.cpp`
   - Genesis creates all expected artifacts and index entries
   - Idempotency: running genesis twice is safe
   - Post-genesis, standard RegisterTypeVersion works for new user types
   - All bootstrap indexes are queryable

### Checklist
- [ ] `src/bootstrap/genesis.h/.cpp` — full genesis commit in PRD dependency order
- [ ] Pre-allocated IDs for all built-in artifacts
- [ ] All 4 built-in TypeDefinition + TypeVersionDefinition artifact pairs
- [ ] All 8 bootstrap IndexDefinition artifacts
- [ ] All built-in ReferenceDefinition artifacts
- [ ] All derived index entries populated
- [ ] `tests/genesis_test.cpp` — creation, idempotency, post-genesis RegisterTypeVersion works
- [ ] Atomic single commit to canonical branch

---

## Phase 9 — gRPC Server & Service Implementation

**Goal**: Wire everything together into a running gRPC server.

### Deliverables

1. **`src/service/artifact_service_impl.h/.cpp`** — ArtifactService gRPC implementation
2. **`src/service/index_service_impl.h/.cpp`** — IndexService gRPC implementation
3. **`src/service/snapshot_transaction_service_impl.h/.cpp`** — SnapshotTransactionService
4. **`src/service/type_registry_service_impl.h/.cpp`** — TypeRegistryService
5. **`src/server.h/.cpp`** — server startup, config, health checks
6. **`src/main.cpp`** — entry point (create storage, run genesis, start server)

7. Error mapping: internal errors -> gRPC status codes + detail messages per the PRD:
   - `INVALID_ARGUMENT` + `ArtifactWriteError` for write validation failures
   - `NOT_FOUND` + `ArtifactNotFoundError` for missing artifacts
   - `ABORTED` + `CommitConflict` for transaction conflicts
   - `NOT_FOUND` + `SnapshotTransactionError` for invalid snapshot/transaction IDs
   - `INVALID_ARGUMENT` + `FetchIndexError` for index fetch errors
   - `INVALID_ARGUMENT` + `RegisterTypeVersionError` for registry errors
   - `UNAVAILABLE` for ID service outage

8. **Tests**: `tests/service_integration_test.cpp`
   - End-to-end tests via gRPC using in-process server with MemoryStorage
   - Register a type, create artifacts, fetch indexes, transaction lifecycle
   - Error responses match PRD specifications

### Checklist
- [ ] `src/service/artifact_service_impl.h/.cpp` — ArtifactService
- [ ] `src/service/index_service_impl.h/.cpp` — IndexService
- [ ] `src/service/snapshot_transaction_service_impl.h/.cpp` — SnapshotTransactionService
- [ ] `src/service/type_registry_service_impl.h/.cpp` — TypeRegistryService
- [ ] `src/server.h/.cpp` — startup, config, health checks
- [ ] `src/main.cpp` — entry point (storage, genesis, server)
- [ ] All gRPC error codes + detail messages match PRD
- [ ] `tests/service_integration_test.cpp` — end-to-end via in-process gRPC server

---

## Phase 10 — ID Allocator Production Integration

**Goal**: Production implementation of `IdAllocatorInterface` using the id-allocator-client-cpp
library. The abstract interface and `MockIdAllocator` were created in P0; this phase
provides the real implementation.

### Deliverables

1. **`src/id/id_allocator.h/.cpp`**:
   - Production implementation of `IdAllocatorInterface` wrapping
     `id_allocator::client::IdAllocatorClient`
   - Double-buffer pre-allocation as described in the PRD
   - `UNAVAILABLE` error when both buffers exhausted
   - Configuration (service address, partition ID, high water mark)

2. Swap `MockIdAllocator` for real allocator in production server configuration

### Checklist
- [ ] `src/id/id_allocator.h/.cpp` — production wrapper around IdAllocatorClient
- [ ] Pluggable in server config (mock for tests, real for production)
- [ ] `UNAVAILABLE` returned when both buffers exhausted
- [ ] Tests for production allocator failure scenarios

---

## Dependency Graph

```
P0 (Scaffolding + ID Allocator Interface)
 |
P1 (Protos + StoredArtifact envelope)
 |
 +---> P2a (Storage Interface + MemoryStorage)
 |       |
 |       +---> P2b (LakeFS) ─── parallel, validates interface ───────────────────────┐
 |       |                                                                            |
 +---> P3 (Path/Key Encoding)                                                        |
 |       |                                                                            |
 |       +---> P4 (Index Storage & Merge + Index Schema Generator)                    |
 |               |                                                                    |
 |               +---> P5 (Transaction Manager) ◄──── depends on P2a + P4             |
 |                       |                                                            |
 |                       +---> P6 (CRUD) ◄──── depends on P5                          |
 |                               |                                                    |
 |                        +------+------+                                             |
 |                        |             |                                              |
 |                  P7a (Proto       P7b (Registry ◄── depends on P7a + P6            |
 |                   Compiler +      Orchestration)                                   |
 |                   Schema Compat)     |                                             |
 |                                  P8 (Genesis)                                      |
 |                                      |                                             |
 |                                  P9 (gRPC Server) ◄────────────────────────────────┘
 |                                      |                (LakeFS backend available here)
 |                                 P10 (ID Allocator Production)
```

### Explicit dependencies per phase

| Phase | Depends on |
|-------|------------|
| P0 | — |
| P1 | P0 |
| P2a | P1 |
| P2b | P2a (interface definition) |
| P3 | P1 |
| P4 | P3 |
| P5 | P2a + P4 |
| P6 | P5 (P4 is transitive via P5) |
| P7a | P1 (protos only — no artifact layer dependency) |
| P7b | P7a + P6 |
| P8 | P7b |
| P9 | P8 + P2b |
| P10 | P6 |

### Parallel tracks after P1

- **Track A** (storage + transactions): P2a -> feeds into P5
- **Track B** (LakeFS de-risk): P2b starts as soon as `StorageInterface` is defined in P2a.
  Validates the real integration while other tracks build domain logic. By P9, LakeFS is a
  proven, drop-in backend.
- **Track C** (encoding + indexes): P3 -> P4 (now includes index schema generator) ->
  feeds into P5/P6.
- **Track D** (registry utilities): P7a can start after P1, parallel with Tracks A/B/C.
  It depends only on proto definitions and libprotoc, not on any artifact layer runtime.
- **Convergence**: P5 is where Tracks A and C converge. P7b is where Track D converges
  with the CRUD engine. P9 is where Track B converges with everything else.
- P10 (ID Allocator production) is independent, any time after P6.

---

## Notes

- **Testing strategy**: Every phase produces unit tests. P2a and P2b share a conformance
  test suite against `StorageInterface`. All upper-layer tests (P3–P9) use `MemoryStorage`
  and `MockIdAllocator`. LakeFS integration tests (P2b) require a running LakeFS instance
  and are marked as integration tests. P10 tests real ID allocator integration.
- **LakeFS de-risking**: P2b is deliberately early because LakeFS is the highest-risk
  dependency. If the merge/conflict semantics don't map cleanly to our interface, we want to
  discover that before the transaction manager and CRUD engine are built on top of assumptions
  that may need to change. Any LakeFS-specific limitations or workarounds found in P2b should
  be documented and may feed back into the `StorageInterface` design.
- **Proto compilation at runtime**: P7a requires the ability to compile .proto source at
  runtime. The protobuf library includes `google::protobuf::compiler` (`libprotoc`) which
  can be used for this. This is a non-trivial dependency: libprotoc is not thread-safe and
  not designed for server-embedded use. P0 should verify the CMake target is available; P7a
  implements the actual compiler with single-threaded serialization. Because P7a depends only
  on P1 (proto definitions), it can start early and run in parallel with Tracks A/C, giving
  ample time to discover and work around libprotoc limitations.
- **Dynamic message handling**: Index objects use runtime-generated schemas. The
  `google::protobuf::DynamicMessage` API is essential and should be explored early (P3/P4).
- **Deterministic protobuf serialization**: Required for index objects. Protobuf's
  `SerializeToString` with `SetDefaultSerializationType(
  io::CodedOutputStream::DETERMINISTIC)` or the `SerializeDeterministic` methods should be
  used.
- **SHA-256 dependency**: Needed for index key hashing. OpenSSL (already a transitive dep
  from gRPC) provides this.

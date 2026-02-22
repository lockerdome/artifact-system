# Layered Artifact System

## Launch Scope (MVP)

### In scope
1. Artifact CRUD with append-only writes and tombstone-based deletes (see Delete semantics and retention).
1. Artifact payloads are structured by registered protobuf schemas, with fields available for indexing; applications may
   still include opaque sections (for example, raw bytes or text/JSON) inside protobuf envelopes they define.
1. Snapshot and transaction-based concurrency model exposed to the App Layer (see Consistency and transaction semantics).
   Snapshots are immutable read-only commit pointers; transactions are mutable read-write branch pointers with snapshot
   isolation. Conflict ownership is defined in the Conflict retry policy.
1. Deterministic index derivation and merges for unique and non-unique indexes; launch uses inline index storage and
   preserves metadata for future sharded layouts.
1. Referential integrity with field-level reference options, write-time validation, and delete-time enforcement
   (restrict/cascade/set-null).
1. Type registry with runtime registration of proto3 schemas, descriptor sets as canonical runtime artifacts, and
   retained custom options including LLM instruction/description annotations at message and field levels. Types, type
   versions, index definitions, and reference definitions are first-class artifacts with their own indexes (see Built-in
   types and bootstrapping).
1. Type version resolution via the current pointer on TypeDefinition, automatically set by RegisterTypeVersion (see Type identity and versioning).
1. Transaction and snapshot API exposed to the App Layer; internal Storage Layer branch and merge mechanics are hidden.
   Callers interact with snapshots (immutable read contexts) and transactions (read-write contexts with snapshot isolation),
   and receive structured conflict responses on commit (Artifact Layer transaction commit, which translates to a merge in
   the Storage Layer).

### Non-goals for launch

#### Post-launch goals
1. Partial-update APIs that avoid full rewrites; any modification writes a full new object and duplicates unchanged data.
1. Index sharding and index migrations (backfills/reindexes).
1. Index where clauses (predicate-based indexing).
1. Caching layers, triggers, and virtual/computed fields.
1. Application-defined payload merges (including CRDT-based merges) beyond index-derived merges.
1. Extended artifact validation beyond proto3 structural checks and referential integrity (semantic/business rules).
1. Compound reference keys and reference lookups by unique indexes (non-artifact_id).
1. Supporting pulling of "part" of an artifact
1. App layer permissions

#### Not planned
1. Cross-repo transactions (atomic commits spanning multiple LakeFS repos).
1. Map field indexing (protobuf map<...> fields); map entries are unordered and ambiguous for index keys/values, so model
   indexable map-like data as repeated entry messages.

### Dependencies and constraints
1. LakeFS provides the storage transaction and conflict model; transactions are limited to a single repo.
1. Append-only history is preserved even when tombstones are written.
1. Deterministic encoding is required for index objects to ensure reliable diffs and merges.
1. ID allocation is handled by a separate service. IDs are pre-allocated in large batches using a double-buffer
   approach: once the front buffer crosses a high-water mark, the back buffer is filled asynchronously. When the front
   buffer is exhausted, front and back swap. This ensures CreateArtifact calls do not block on the ID service under
   normal load. If both buffers are exhausted (ID service prolonged outage), CreateArtifact returns an unavailable error.
   IDs are opaque uint64 values; gaps are expected (for example, if the server crashes after allocating an ID but before
   committing the artifact) and are benign.
1. GCS enforces a 1024-byte maximum object name (UTF-8 encoded). The full GCS path includes the lakeFS installation
   prefix and the object key. Index object paths use content-addressed hashing (see Index physical storage) to guarantee
   a fixed-length path segment regardless of key field sizes.
1. Index metadata must preserve an upgrade path to future sharded layouts.

## Layer overview
1. Storage Layer: durable object storage with versioned commits, branches, merges, and conflict detection. LakeFS is the
   current implementation, but we could use any system with similar semantics (for example, Dolt). No domain rules.
1. Artifact Layer: canonical API for artifact CRUD and ID allocation, schema/type registry, index derivation, and index
   conflict resolution.
1. App Layer: domain-specific rules, permissions, and workflows around artifacts. An example App Layer service is an
   Artifact Viewer.

## Storage Layer

The Storage Layer provides transactions, versioning and storage. LakeFS is the current implementation. It does not
encode artifact schemas or domain logic.

### Integration with LakeFS

LakeFS is provisioned via the `provision.sh` script, which creates a MIG, Postgres database (HA), Load Balancer, Image,
Instance Template, Health Check, Spark vacuum service, VPC, and related resources for a highly available cluster. The
script uses two zones with the MIG for high availability. It also creates a Cloud Storage bucket named
`lakefs-data-{project-id}`. Set `PROJECT_ID` before running the script and verify that the IP ranges are compatible with
the broader network.

The admin account secret is stored in Secret Manager as `lakefs-secret-access-key`; the access key ID is configured via
the `ACCESS_KEY_ID` value in the provision script. Access to the LakeFS admin portal requires an IAP/SSH tunnel with
local port forwarding to one of the instances. The provision script prints connection instructions on success.

A single LakeFS repo is used for all artifacts. All objects that participate in the same transactions must be in the same
repo (see Dependencies and constraints). In the future, repo-scoped artifacts may be supported (for example, isolating a
specific tenant's artifacts into a dedicated repo) to reduce merge contention at scale. The Spark vacuum process removes commits that are not recent and not pinned by a
tag or referenced as a branch head. The canonical branch head is always pinned. Ephemeral write branches that are not
merged within 7 days are eligible for cleanup. To preserve older commits beyond the default retention window, pin them
with tags or adjust the retention configuration.

### Conflict model

LakeFS does not perform content-aware merges; when objects diverge on two branches, it reports a conflict. Artifact
payload conflicts are rare, but index objects — which are updated on every artifact write — conflict frequently under
concurrent writes. Pre-merge hooks are not used for conflict resolution; instead, the artifact layer handles conflicts
post-merge. When a merge attempt returns a conflict, the artifact layer reads the conflicting objects, resolves index
conflicts using deterministic merge logic, and retries the merge. This repeats until the merge succeeds or the retry
limit is reached. The retry policy is defined below.

#### Conflict retry policy (MVP)

**Ownership**
1. Artifact layer (server) retries merges only for **non-unique index** object conflicts that are resolvable by deterministic index merge logic.
1. Unique index conflicts (including cases where a unique index would contain more than one item) are returned to the caller.
1. Artifact payload conflicts are never auto-resolved; they are returned to the caller (app layer/client) immediately.

**Eligibility**
1. Retry only when all reported conflicts are **non-unique index** objects.
1. If any payload/object conflict outside indexes is present, or any unique index conflict is detected, abort retries and return a conflict response.

**Attempts and backoff**
1. Maximum attempts: 5 total merge attempts (initial attempt + up to 4 retries).
1. Backoff: exponential with jitter, starting at 100ms and capped at 2s (for example: 100ms, 200ms, 400ms, 800ms, 1600–2000ms).
1. Backoff applies between retries only; successful merges return immediately.

**Idempotency requirements**
1. Index merge logic must be deterministic and idempotent given the same (base, ours, theirs) inputs.
1. Artifact writes within a transaction use snapshot isolation to prevent lost updates. Conflicts may be detected at two
   points: (a) when a write's sub-branch is merged into the transaction branch (concurrent writes within the same
   transaction), and (b) when the transaction branch is merged into the canonical branch (Artifact Layer transaction
   commit). Both merge points use the same retry policy.

**Conflict response payload**
When retries are aborted or exhausted, return a structured conflict response containing:
1. conflict_type: one of {INDEX_CONFLICT, PAYLOAD_CONFLICT, REFERENTIAL_INTEGRITY_VIOLATION}.
1. retryable: boolean indicating whether the server would retry (false when exhausted or payload conflict).
1. attempts: number of attempts performed.
1. artifacts/indexes involved: artifact_id for payload conflicts; index key (key_type + encoded key) for index conflicts;
   for referential integrity violations include the target artifact_id, the reference key_type, and the referencing
   artifact_ids.
1. version_ids: base, ours, theirs Storage Layer commit IDs when available.

**Exhaustion behavior**
1. If the retry limit is reached while resolving index conflicts, return the conflict response with retryable=false.
1. Callers may create a new transaction, re-read state, resolve conflicts at the application level, and retry the
   operation.

## Artifact Layer

This layer builds on the Storage Layer and provides shared artifact functionality: artifact schemas and types, ID
allocation, indexes, index conflict resolution, and type metadata. The type system is self-describing: TypeDefinition,
TypeVersionDefinition, IndexDefinition, and ReferenceDefinition are all stored as first-class artifacts with their own
indexes, bootstrapped via a genesis commit (see Built-in types and bootstrapping). Application-specific logic and
permissions live in the App Layer.

### Necessary features for launch

1. Integration with LakeFS
1. Indexes for looking up artifacts (and ability to resolve conflicts)
1. Artifact Type registry (with version and schema support)
1. Referential integrity validation and delete-time enforcement
1. Genesis bootstrap for built-in types and indexes

### Indexes for looking up artifacts

Concepts such as tags and groups are not separate primitives; they are implemented as indexes (for example, a "tags"
index is a non-unique index keyed by tag value). The specific fields indexed depend on the artifact type, but the
underlying index storage format is uniform. Index objects are subject to merge conflicts because
any concurrent artifact write can update the same index object. Conflict resolution uses a three-way diff between the
merge base and both heads (see Conflict model).

Indexes are derived from artifacts by artifact layer logic and are never manually set. They only conflict on merge.
The commit workflow reads the latest index state, applies adds/removes derived from the artifact changes, and writes
back the updated index object(s).

#### Index definition

Index definitions are expressed as protobuf messages (IndexDefinition) and attached to artifact types via a
message-level custom option (indexes). The IndexDefinition schema describes the fields below. Each IndexDefinition is
stored as an artifact (of the built-in IndexDefinition type) and receives its own artifact_id via the standard ID
allocation path during type registration. That artifact_id is used as the key_prefix in index storage paths. Index
definition artifact_ids are resolved at runtime by extracting key_type values from the TypeVersionDefinition's descriptor
set custom options and looking them up via the `index_key_type_unique` index. IndexDefinition artifacts are created as
part of the RegisterTypeVersion transaction before any user artifacts of that type exist.

An index definition includes:
1. key_type: the name of the index and the identifier passed to fetch index calls. key_type must be globally unique
   across all registered index definitions; this uniqueness constraint is enforced by a self-referential unique index
   (`index_key_type_unique`) declared on the IndexDefinition message itself. Because IndexDefinition instances are stored
   as artifacts, the standard unique index merge semantics (see Index merge semantics) enforce at most one
   IndexDefinition per key_type value. This eliminates the need for special-case registry logic and ensures the
   constraint is enforced consistently through the same path as all other unique indexes.
2. key: the fields that partition the index. Different key values on the same index type map to different index
   objects.
3. order fields: fields that determine the sort order. Each order field must declare a direction (ASC or DESC) based on
   the field type's sort order. The special field name artifact_id refers to the artifact ID (not a payload field) and
   must be included as an order field to make each row uniquely identifiable. In the MVP, artifact_id is required but
   does not need to be the final order field. This is a simplification of a general unique constraint on index rows.
4. unique: whether the index enforces at most one artifact ID per index key.

Where clauses are deferred to post-MVP. When introduced, IndexDefinition will gain an optional predicate expression; the
expression AST is shared with virtual/computed fields and is a near-term design decision (see Post-MVP notes).

#### Index merge semantics

When a merge reports a conflicting index object, we perform a three-way merge using the merge base and both heads. Each
index object is treated as a set of entries keyed by artifact ID with associated order field values. For each branch we
compute adds (head minus base) and removes (base minus head). If the same artifact ID exists in both but with different
order values, treat that as a remove and an add. We apply all removes to the base, then apply all adds, de-duplicate by
artifact ID, and re-sort by the configured order fields. This yields a
deterministic result and is idempotent across retries. If a unique index (see Index definition) ends up with more than
one item, the merge
fails and the conflict is returned to the calling service for resolution. Unmerged branches may temporarily violate
uniqueness; uniqueness is enforced when changes are merged into the canonical branch.

#### Index physical storage

Index objects are stored under indexes/{key_prefix}/{key_hash}. The key_prefix is the uint64 IndexDefinition artifact ID
encoded as base64 using the URL-safe alphabet of its big-endian uint64 bytes without padding (always 11 characters). The
key_hash is the SHA-256 digest of the deterministically encoded key bytes (see Index key encoding), rendered as base64
using the URL-safe alphabet without padding (always 43 characters). This content-addressed scheme produces a fixed-length
path (indexes/ [8] + key_prefix [11] + / [1] + key_hash [43] = 63 bytes) regardless of key field sizes, satisfying the
GCS 1024-byte object name limit (see Dependencies and constraints). Because the path is a hash, the actual key field values are stored in the
index object payload (see Index object representation).

#### Index key encoding

Index keys use a deterministic binary encoding independent of protobuf wire encoding. The encoded bytes are the
pre-image for the SHA-256 hash used in the index object path (see Index physical storage). This encoding is the only
source of truth for key identity in content-addressed paths; we do not rely on the protobuf serialization of stored
index objects to represent logical keys. Deterministic encoding is critical: different encodings of the same logical key
would produce different hashes and thus different paths, creating orphaned index objects.

Key field values are encoded as little-endian to enable zero-copy access on common hosts. This differs from the
big-endian encoding used for ID-based path segments (artifact_id in paths, key_prefix in index paths), which prioritize
human-sortable lexicographic order.

Index value payloads (order-field columns) are stored as typed protobuf fields and use standard protobuf wire encoding,
not the custom binary encoding defined here; see Index object representation for details.

Ordering comparisons in index merge logic and sorted operations use the field's native type semantics (numeric order for
numbers, lexicographic for strings/bytes) on decoded protobuf values. All comparisons are decode-then-compare; ordering
is independent of the key hash encoding.

**Key encoding rules**

To produce the hash pre-image, concatenate key field encodings in the declared order using the following rules.

1. int32, sint32, sfixed32: 4-byte two's-complement little-endian.
1. uint32, fixed32: 4-byte unsigned little-endian.
1. int64, sint64, sfixed64: 8-byte two's-complement little-endian.
1. uint64, fixed64: 8-byte unsigned little-endian.
1. bool: 1 byte (0x00 or 0x01).
1. enum: 4-byte signed little-endian (int32 encoding).
1. float: IEEE 754 binary32, little-endian; double: IEEE 754 binary64, little-endian. NaN values are rejected
   (`NAN_IN_INDEXED_FIELD`) and -0 is normalized to +0 during index derivation. Because artifact writes and index
   derivation occur within the same transaction, a NaN in an indexed float field rejects the entire write (artifact
   payload and all derived indexes).
1. string: UTF-8 bytes prefixed by an unsigned varint length (base-128, LSB-first, minimal encoding).
1. bytes: raw bytes prefixed by an unsigned varint length (base-128, LSB-first, minimal encoding).

Varint length prefixes must use minimal encoding: the shortest base-128 representation with no leading zero groups. This
is required for deterministic encoding; non-minimal varints must be rejected during index derivation
(`NON_MINIMAL_VARINT`).

1. If a key field references a scalar sub-field of a message, encode the referenced scalar using the same rules.

#### Index object representation

Index objects must be encoded deterministically to make diffs and merges reliable. The stored payload is the full
`Index_*` proto3 message, which contains the key fields (via an embedded `IndexKey_*` sub-message) and the value fields
(via an embedded `IndexValue_*` sub-message with one `repeated` typed field per order column, forming parallel arrays in
columnar layout). Key fields are always stored in the payload because index object paths use a content-addressed hash and
the key cannot be reconstructed from the path (see Index physical storage). Rows are ordered by the order fields. All
columns have the same length, equal to the row count. Using typed proto
fields (rather than raw bytes) means serialization, deserialization, and type safety are handled by the protobuf runtime;
no custom binary codec is needed for the stored payload. Deterministic encoding is achieved by using protobuf
deterministic serialization (sorted map keys, fixed field order).

#### Index fetch behavior (MVP)

The index fetch API (FetchIndex) provides a raw fetch of the materialized index results for a specific index key. There is no
pagination, cursoring, filtering, or query-time predicate evaluation in the MVP. Predicate-based indexing (where clauses) is
post-MVP; FetchIndex returns the stored index state only.

**Inputs**
1. key_type: identifies which index definition to query.
1. key values: the complete set of key fields defined by the index, provided as typed values. All key fields are
   required; partial keys are rejected.

**Execution semantics**
1. The artifact layer locates the index object for (key_type, encoded key values) and reads its current state. If issued
   within a snapshot, reads use the snapshot's Storage Layer commit pointer. If issued within a transaction, reads use the
   transaction's ephemeral branch head (which includes the transaction's own writes and is isolated from concurrent
   Artifact Layer commits to the canonical branch). If issued without a snapshot or transaction context, reads use the
   canonical branch head.
1. Results are returned in the deterministic order defined by the index's order fields. Ordering stability is defined
   by the index definition and does not vary per query.
1. No query-time filters are applied. The query returns exactly the contents of the index object.

**Response shape**
The response payload is the concrete, generated index message for the queried index (for example,
`Index_DataFrameArtifact_by_repo_created_by`). The server returns the index object as stored, using the
index-specific protobuf schema.

1. The key message corresponds to the index key fields.
1. The value message contains typed `repeated` fields for each order column defined by the index, in the declared order;
   see Index object schema for the generated layout. The MVP does not implicitly embed artifact payloads in index values
   unless the index schema explicitly defines such fields.
1. Empty or tombstoned indexes return an index message with zero entries.

The response may also include the Storage Layer commit ID used for the read, for debugging or consistency checks.

**Uniqueness**
1. Unique index responses contain zero or one entry (see Index definition for the uniqueness invariant).

#### Index sharding (planned)

When indexes become large, shard them by bucket and store a small manifest object that lists all bucket objects for an
index key. Bucket naming should be deterministic (for example, a fixed-width prefix of a hash) so that reads can page
over buckets predictably. The manifest is the canonical entrypoint for reads. The MVP index metadata must preserve a
forward-compatible upgrade path so that individual indexes can be dynamically migrated from single-object to sharded
layout without rewriting all index consumers.

### Object namespaces and paths

Object paths are a private implementation detail and must not be exposed to end-users. The top-level namespaces are:

1. `artifacts/{artifact_id}` — artifact payloads, including built-in definition artifacts (TypeDefinition,
   TypeVersionDefinition, IndexDefinition, ReferenceDefinition). The artifact_id is a uint64 encoded as base64 using the
   URL-safe alphabet of its big-endian bytes without padding. Type definitions, type versions, index definitions, and
   reference definitions are all stored in this namespace and are located via indexes (see Built-in types and bootstrap
   indexes).
2. `indexes/{key_prefix}/{key_hash}` — index objects. The key_hash is a SHA-256 content-addressed hash of the encoded
   key bytes; path encoding is defined in Index physical storage.

### Artifacts

Artifacts are stored as opaque payloads defined by Types. Writes occur within transactions and are committed atomically
(an Artifact Layer transaction commit translates to a Storage Layer merge into the canonical branch). Conflict ownership
(payload vs. index) is defined in the Conflict retry policy. In the future, we can add application-defined merge logic
per artifact type.

#### Artifact contract (minimum)

Artifacts are defined by (artifact_id, type_name, version_id, payload).
1. artifact_id: opaque uint64 allocated by a separate ID allocation service.
2. type_name: the fully-qualified proto message name. Must resolve to a registered TypeDefinition artifact via the
   `type_name_unique` index.
3. version_id: optional uint64 artifact_id of a TypeVersionDefinition. If omitted, the artifact layer resolves to the
   TypeDefinition's current_version_id (see Type identity and versioning). The resolved TypeVersionDefinition must have a
   type_id matching the TypeDefinition's artifact_id.
4. payload: serialized protobuf message (binary wire format) for the resolved type version.

The artifact_id is metadata and does not need to be duplicated in the payload. If a type schema includes an id field, the
artifact layer should validate that it matches the artifact_id.

On create/update/delete, the artifact layer checks the TypeDefinition's mutation-restriction flags. If `deny_create` is
true, CreateArtifact is rejected (`MUTATION_DENIED`). If `deny_update` is true, UpdateArtifact is rejected
(`MUTATION_DENIED`). If `deny_delete` is true, DeleteArtifact is rejected (`MUTATION_DENIED`). The artifact layer
validates the payload using standard proto3 structural validation against the message identified by type_name in the
TypeVersionDefinition's descriptor set (`PAYLOAD_VALIDATION_FAILURE` on failure), and derives index entries from the
index declarations in the descriptor set's custom options. Artifact validation is limited to proto3 structural checks
and referential integrity (see below); semantic or business validation is out of scope for launch. All CRUD write-time
validation failures return gRPC status `INVALID_ARGUMENT` with an `ArtifactWriteError` detail message; see CRUD write
error response for the full specification. Responses return the
resolved version_id (the TypeVersionDefinition artifact_id). All writes occur within a transaction; concurrent write
conflicts are detected at Artifact Layer transaction commit (see Consistency and transaction semantics). Reads return
payload bytes plus type_name and version_id. Partial updates are out of scope for launch.

**Internal bypass of mutation restrictions**: Certain artifact layer operations require creating, updating, or deleting
artifacts whose types have `deny_create`, `deny_update`, or `deny_delete` set, as part of their internal logic. For
example, RegisterTypeVersion creates TypeVersionDefinition and IndexDefinition artifacts (bypassing `deny_create`),
updates the tail TypeVersionDefinition's `next_version_id` pointer (bypassing `deny_update`), and updates the
TypeDefinition's `current_version_id` and mutation-restriction flags (bypassing `deny_update`). These internal operations
bypass the public mutation-restriction checks. Internal bypasses are limited to system-managed bookkeeping performed by
the artifact layer itself; external callers cannot trigger them through the public CRUD API. This is a stopgap
mechanism; post-MVP field-level mutability will provide a principled way to declare which fields are mutable on
otherwise-restricted types (see Post-MVP notes).

The artifact layer stores payload bytes as provided and does not reserialize them. This preserves unknown fields and
avoids any reliance on canonical protobuf binary encodings. Text/JSON formats are not used for storage.

#### Referential integrity (MVP)

Referential integrity is expressed via field-level options on `uint64` fields that reference other artifacts by
artifact_id. Each reference declaration is materialized as a ReferenceDefinition artifact at registration time (see
Built-in types and bootstrapping). The descriptor set options are the source of truth; ReferenceDefinition artifacts are
derived materializations for reverse lookup and enforcement.

**Declaration and registration rules**
1. The references option is valid only on `uint64`, `optional uint64`, or `repeated uint64` fields. The registry rejects
   references on any other field type (`INVALID_REFERENCE_DECLARATION`).
1. `on_delete` is required; `ON_DELETE_UNSPECIFIED` is rejected (`INVALID_REFERENCE_DECLARATION`).
1. `SET_NULL` is valid on `optional` or `repeated` fields; it is rejected on implicit-presence scalars
   (`INVALID_REFERENCE_DECLARATION`).
1. Implicit-presence scalar reference fields always validate their value (including defaults); use `optional` to allow
   null. Repeated reference fields treat an empty list as null.
1. `target_type_name` must resolve to an existing TypeDefinition via the `type_name_unique` index.
1. A covering index is required on the referencing message: exactly one index must exist where the reference field is
   the **sole key**. The covering index may be unique or non-unique. In the MVP, the covering index must not include a
   where clause; post-MVP may allow a single `IS_SET` predicate to gate optional reference fields. For repeated reference
   fields, the covering index has no where clause.
1. Multiple reference fields are allowed per message; each is validated independently.

**Write-time validation**
1. On Create/Update, if a reference field is set, the artifact layer reads the referenced artifact by artifact_id and
   verifies it exists (`REFERENCE_TARGET_NOT_FOUND`), is not tombstoned (`REFERENCE_TARGET_TOMBSTONED`), and its
   type_name matches `target_type_name` (`REFERENCE_TARGET_WRONG_TYPE`).
1. If a reference field is unset (optional), it is treated as null and does not require validation.
1. For repeated reference fields, each value is validated and the list must not contain duplicates; duplicate values are
   rejected on Create/Update (`REFERENCE_DUPLICATE_VALUE`).
1. References are enforced on create/update; existing stored artifacts are not revalidated until they are updated.

**Delete-time enforcement**
1. On Delete, the artifact layer resolves the artifact's type_name and fetches all ReferenceDefinition artifacts from the
   `references_by_target_type` index for that type.
1. For each ReferenceDefinition, it fetches the covering index with key = deleted artifact_id to list referencing
   artifacts.
1. Enforcement by `on_delete`:
   1. **RESTRICT**: reject the delete (`REFERENCE_DELETE_RESTRICTED`) if any referencing artifacts exist that are not
      already scheduled for delete in the current transaction.
   1. **CASCADE**: delete each referencing artifact (recursively applying referential integrity).
   1. **SET_NULL**: update each referencing artifact to clear the reference field (for repeated fields, remove the
      referenced value from the list).
1. All cascades and nullify updates run in the same internal transaction (same ephemeral branch) as the original delete
   and are merged atomically.
1. Cascading cycles are handled by tracking scheduled deletes and skipping already-marked artifacts; this prevents
   infinite recursion while allowing mutually-referencing artifacts to be deleted in the same transaction.
1. Referential integrity violations return conflict_type = REFERENTIAL_INTEGRITY_VIOLATION.

**Forward compatibility**
1. MVP references are `uint64` artifact_ids validated by GetArtifact + type_name checks. Future versions may allow
   references by unique index (for example, a compound key struct). This would add a `target_index_key_type` to
   ReferenceOption/ReferenceDefinition and require the reference field to match the target index key schema.

#### Delete semantics and retention

Deletes are logical tombstones. A delete writes a new version at the same artifact key with an empty payload within the
current transaction. The delete is finalized when the Artifact Layer transaction is committed (merged in the Storage
Layer).

1. Empty payloads are reserved for tombstones. The artifact layer rejects zero-length payloads for live artifacts
   (`EMPTY_PAYLOAD`); if a type needs to represent an "empty" value, include a sentinel field or wrap it in an envelope
   message.
1. Index handling: deleting an artifact removes all derived index entries for that artifact ID (as if the artifact no
   longer qualifies for any index entries). Unique indexes free the slot. If an index becomes empty, it is tombstoned by
   storing an index object with an empty value portion (row_count = 0 and no value entries) so merges can detect
   deletions.
1. Referential integrity: deletes may be rejected (`REFERENCE_DELETE_RESTRICTED` for RESTRICT), cascade, or nullify
   references based on reference field options; enforcement occurs in the same internal transaction (see Referential
   integrity and CRUD write error response).
1. Reads treat tombstones as not found for standard Get. There is no public restore or audit API in the MVP; to
   "undelete" a caller writes a new payload for the tombstoned artifact within a transaction, creating a new version in
   history.
1. Retention: history is preserved as LakeFS commits but is subject to LakeFS GC/vacuum. To guarantee audit/restore
windows, pin commits/tags or configure retention accordingly. Hard delete/purge is out of scope for launch.

#### Consistency and transaction semantics

The Artifact Layer exposes snapshot and transaction primitives to the App Layer as the primary concurrency model.
Storage Layer branch and merge mechanics are hidden; callers interact with snapshots and transactions.

**Terminology note**: "commit" in this section has two meanings depending on the layer:
1. **Storage Layer commit** — a LakeFS commit, an immutable point-in-time state of the repository.
2. **Artifact Layer transaction commit** — the act of finalizing and persisting a transaction's writes, which translates
   to a Storage Layer merge of the transaction's ephemeral branch into its parent (or the canonical branch).

##### Snapshots

A snapshot is an immutable, read-only pointer to a Storage Layer commit. Snapshots do not require a Storage Layer branch;
they reference a commit directly.

1. `CreateSnapshot(parent_id?)` returns a snapshot_id. If parent_id is omitted, the snapshot points to the canonical
   branch head. If parent_id is a transaction ID, the snapshot points to the transaction's current head Storage Layer
   commit.
1. All reads within a snapshot (GetArtifact, BatchGetArtifacts, FetchIndex) are performed against the snapshot's Storage
   Layer commit, providing a consistent, point-in-time view.
1. Snapshots are lightweight and immutable. They serve as the read context for callers that need consistent multi-read
   operations without performing writes.

##### Transactions

A transaction is a mutable, read-write context backed by an ephemeral Storage Layer branch.

1. `CreateTransaction(parent_id?)` returns a transaction_id. If parent_id is omitted, the ephemeral branch is forked from
   the canonical branch head. If parent_id is a snapshot ID, the branch is forked from that snapshot's Storage Layer
   commit. If parent_id is a transaction ID, the branch is forked from the parent transaction's current head Storage Layer
   commit, creating a sub-transaction.
1. Reads within a transaction use the transaction's ephemeral branch head, which includes the transaction's own writes.
   This provides read-after-write visibility within the transaction. Snapshot isolation applies relative to other
   concurrent Artifact Layer transaction commits: reads are isolated from changes that other transactions commit to the
   canonical branch after this transaction was created. This gives the behavior expected by application developers and
   avoids subtle bugs where transactional logic sees partially-committed external state.
1. Each write operation (Create/Update/Delete) within a transaction is executed as a sub-branch of the transaction's
   ephemeral branch: the Artifact Layer forks a child branch from the transaction branch head, stages the artifact payload
   and all derived index updates on the child branch, commits them as a single Storage Layer commit, and merges the child
   branch back into the transaction branch. This ensures that each logical write is atomic (artifact + indexes are committed
   together) and that concurrent writes within the same transaction do not interfere via a shared staging area. Without this
   isolation, two concurrent writes on the same transaction branch could race: both stage objects, and whichever calls the
   Storage Layer commit first would capture the other's uncommitted staged changes. The sub-branch-per-write pattern
   serializes writes through the merge step.
1. `CommitTransaction(transaction_id)` finalizes the transaction: the ephemeral branch is merged into its parent. For a
   top-level transaction, this is a merge into the canonical branch. For a sub-transaction, this is a merge into the
   parent transaction's ephemeral branch. Conflicts detected during the Storage Layer merge are handled per the Conflict
   retry policy.
1. `RollbackTransaction(transaction_id)` abandons the transaction without merging. The ephemeral branch is deleted.
1. Delete operations that cascade or nullify references perform all dependent writes in the same transaction (same
   sub-branch), ensuring the artifact deletion and all referential integrity side-effects are committed atomically.

##### Recursive (nested) transactions

Transactions support nesting: a sub-transaction can be created by providing a parent transaction ID to
`CreateTransaction`. Sub-transactions are independent ephemeral branches forked from the parent transaction's head.

1. Committing a sub-transaction merges its ephemeral branch into the parent transaction's branch (not the canonical
   branch).
1. Rolling back a sub-transaction discards its branch without affecting the parent.
1. The parent transaction can continue to accumulate writes from multiple sub-transactions before committing to the
   canonical branch.
1. This model supports patterns such as retryable steps within a larger workflow, parallel sub-tasks that merge into a
   shared transaction, and long-running processes with staged checkpoints.

##### Implicit transactions

For convenience, single write operations (Create/Update/Delete) that are not issued within an explicit transaction are
wrapped in an implicit transaction: the Artifact Layer creates an ephemeral branch from the canonical branch head, stages
the artifact and all derived index updates, commits them as a single Storage Layer commit, and merges the ephemeral
branch back into the canonical branch before returning success. Since implicit transactions contain exactly one write,
there is no concurrency on the staging area and no sub-branch is needed. This preserves simple semantics for single
operations while allowing callers to opt into explicit transactions for multi-write atomicity.

##### General semantics

1. Read-after-write within a transaction: reads within a transaction see the transaction's own writes. Each write is
   committed to the transaction's ephemeral branch (via the sub-branch-per-write pattern), and subsequent reads use the
   branch head.
1. Snapshot isolation from concurrent commits: a transaction is isolated from other transactions' Artifact Layer commits
   to the canonical branch. The transaction's reads see the canonical branch state as of when the transaction was created,
   plus the transaction's own writes.
1. Read-after-commit: once an Artifact Layer transaction commit succeeds, subsequent reads against the canonical branch
   (or new snapshots/transactions) reflect the committed writes.
1. Multi-key and cross-artifact atomic writes are supported within a single transaction. All writes in a transaction are
   merged atomically on Artifact Layer transaction commit.
1. Concurrent writes within the same transaction: the sub-branch-per-write pattern serializes concurrent writes through
   the merge step into the transaction branch. Two concurrent writes will each operate on their own sub-branch; the second
   to merge will see the first's changes in the transaction branch and may conflict if they touch the same objects (for
   example, the same index). This is handled by the same Conflict retry policy as inter-transaction conflicts.
1. Concurrent transactions may diverge on separate ephemeral branches; conflict ownership is defined in the Conflict
   retry policy and conflicts surface at Artifact Layer transaction commit time.
1. If a write fails mid-transaction (for example, index derivation rejects a NaN float key — `NAN_IN_INDEXED_FIELD`),
   the sub-branch for that write is abandoned without merging into the transaction branch. The transaction remains open
   and the caller may retry the write or roll back the transaction. See CRUD write error response for the full error
   specification.
1. Unique index enforcement is described in Index definition and Index merge semantics.

##### Cleanup and TTL for abandoned transactions

If a caller crashes without calling CommitTransaction or RollbackTransaction, the ephemeral branch is orphaned.

1. Ephemeral branches that have not been committed or rolled back within a configurable TTL (default: 7 days) are
   eligible for cleanup. The artifact layer periodically scans for expired ephemeral branches and deletes them.
1. The LakeFS Spark vacuum process also removes Storage Layer commits on branches that are not recent and not pinned by a
   tag or referenced as a branch head, providing a secondary cleanup mechanism.

##### App Layer guidance

1. **Read lifetimes**: Snapshots and transactions provide a consistent read context. Callers should be aware that a
   snapshot represents a point-in-time view; long-lived snapshots may read stale data. For use cases requiring fresh
   reads, create a new snapshot or transaction.
1. **Multiple reads**: When an operation depends on reading multiple artifacts or indexes consistently, perform all reads
   within a single snapshot or transaction to guarantee they see the same state.
1. **Multi-write transactions**: When an operation requires multiple writes that must be atomic (or writes that depend on
   consistent reads), use an explicit transaction. The transaction guarantees that all writes are committed together and
   that reads within the transaction see a consistent snapshot.
1. **BatchGetArtifacts limitation**: BatchGetArtifacts returns artifacts by ID but does not follow references. When
   artifacts contain references to other artifacts, the caller must issue additional reads to resolve references. A
   reference-aware batch fetch is a planned follow-up.

#### Artifact envelope (optional)

If a composite artifact message is needed for APIs, we can assemble it without duplicating stored data. The stored
payload is the type-specific protobuf message; the artifact_id and type metadata live outside the payload. A caller can
request a typed envelope such as:

```proto
message DataFrameArtifactRecord {
  uint64 id = 1;
  DataFrameArtifact value = 2;
}
```

The service can construct this record from the stored payload bytes and the path-derived artifact_id/type metadata. If a
type schema includes an id field, it must match the artifact_id.

### Types (artifact type registry)

#### Built-in types and bootstrapping

The artifact system uses four built-in artifact types to describe itself. All definition objects are first-class
artifacts stored in the `artifacts/` namespace and queryable via standard indexes. Their schemas are compiled into the
artifact layer binary but are also stored as artifacts so the system is fully self-describing.

1. **TypeDefinition**: represents a logical artifact type. One TypeDefinition artifact exists per type_name. Contains the
   type_name (which is the fully-qualified proto message name) and an optional current_version_id pointer to the default
   TypeVersionDefinition. Also carries three mutation-restriction flags: `deny_create` (when true, CreateArtifact is
   rejected with `MUTATION_DENIED`), `deny_update` (when true, UpdateArtifact is rejected with `MUTATION_DENIED`), and
   `deny_delete` (when true, DeleteArtifact is rejected with `MUTATION_DENIED`); see CRUD write error response. All default to false. These flags may be set at type registration time and may
   only be tightened (false to true) on subsequent registrations, never loosened (see RegisterTypeVersion). Built-in
   definition types (TypeDefinition, TypeVersionDefinition, IndexDefinition, ReferenceDefinition) set `deny_create`
   to true because their artifacts are created exclusively through dedicated registry operations (e.g.,
   RegisterTypeVersion), not through the public CreateArtifact API. TypeDefinition's own TypeDefinition has
   `deny_create = true, deny_update = true, deny_delete = true`; updates to TypeDefinition artifacts (setting
   `current_version_id` and tightening mutation-restriction flags) are performed exclusively by RegisterTypeVersion
   using an internal bypass of the `deny_update` restriction (see Internal bypass of mutation restrictions).
2. **TypeVersionDefinition**: represents a specific version of a type. Contains the type_id (artifact_id of the parent
   TypeDefinition), the compiled FileDescriptorSet, the original .proto source, and optional `previous_version_id` /
   `next_version_id` pointers that form a doubly-linked list of versions per type. Multiple TypeVersionDefinition
   artifacts may exist for a single TypeDefinition. TypeVersionDefinition has `deny_create = true, deny_update = true,
   deny_delete = true` on its TypeDefinition; artifacts cannot be created, modified, or deleted through the public
   CRUD API (creation is handled by RegisterTypeVersion). The
   `next_version_id` pointer is an exception managed via an internal bypass: the artifact layer updates the tail
   version's `next_version_id` when a new version is registered (see RegisterTypeVersion and Internal bypass of
   mutation restrictions). Metadata such as index definitions, actions, viewer endpoints, and LLM instructions are
   attached as custom options within the descriptor set and are not denormalized onto the TypeVersionDefinition message
   (see Custom options as metadata).
3. **IndexDefinition**: represents a single index. Contains the key_type, key fields, order fields, and unique flag.
   Where clauses are post-MVP. IndexDefinition has `deny_create = true, deny_update = true, deny_delete = true` on its
   TypeDefinition; artifacts cannot be created, modified, or deleted through the public CRUD API (creation is handled
   by RegisterTypeVersion). Each IndexDefinition
   artifact receives an artifact_id that is used as the key_prefix in index storage paths (see Index physical storage).
4. **ReferenceDefinition**: represents a single referential integrity declaration. Contains the referencing type, target
   type, reference field, covering index key_type, and on_delete behavior. key_type is a globally unique identifier;
   the recommended format is `{referencing_type_name}.{field_name}`. The unique key_type is used by the registry to
   detect duplicates across type versions without scanning all reference definitions. ReferenceDefinition has
   `deny_create = true, deny_update = true, deny_delete = true` on its TypeDefinition; artifacts cannot be created,
   modified, or deleted through the public CRUD API (creation is handled by RegisterTypeVersion when reference field
   options are detected).

All types (including user-defined artifact types) are stored in LakeFS so they are tied to the repo state. This enables
migration transactions (post-launch): a new branch can add an index, backfill it for existing data, and merge only when
the index is consistent with all changes since the fork point. The same mechanism supports index removal and schema
migrations.

#### Bootstrap indexes

The built-in types declare the following indexes on themselves:

| Index key_type | On type | Key | Unique | Purpose |
|----------------|---------|-----|--------|---------|
| `index_key_type_unique` | IndexDefinition | `[key_type]` | yes | Enforces globally unique index names; enables key_type to IndexDefinition artifact lookup |
| `type_name_unique` | TypeDefinition | `[type_name]` | yes | Enforces one TypeDefinition per type_name; enables type resolution |
| `all_types` | TypeDefinition | `[]` | no | Lists all registered type artifact_ids |
| `type_versions_by_type` | TypeVersionDefinition | `[type_id]` | no | Lists all version artifact_ids for a type |
| `all_index_definitions` | IndexDefinition | `[]` | no | Lists all registered index definition artifact_ids |
| `reference_key_type_unique` | ReferenceDefinition | `[key_type]` | yes | Enforces globally unique reference identifiers; used by the registry to de-duplicate reference definitions |
| `references_by_target_type` | ReferenceDefinition | `[target_type_name]` | no | Finds references targeting a given type |
| `all_reference_definitions` | ReferenceDefinition | `[]` | no | Lists all registered reference definition artifact_ids |

These indexes are created during genesis bootstrapping and use the same storage and merge mechanics as all other indexes.

#### Genesis bootstrap

The artifact layer initializes the system with a single atomic genesis commit that creates all built-in type artifacts,
index definition artifacts, and their derived index entries. The genesis transaction does not use the normal
RegisterTypeVersion code path because circular dependencies between the built-in types make the standard validation and
derivation pipeline impossible at initialization time. Instead, the built-in artifacts and indexes are authored directly
with pre-allocated IDs. This is analogous to how a database bootstraps its own catalog tables.

The genesis commit creates artifacts in the following dependency order:

1. **IndexDefinition type** (TypeDefinition + TypeVersionDefinition artifacts): must be first because every other type
   declares indexes, and creating index definitions requires the IndexDefinition type to exist.
2. **`index_key_type_unique`** (IndexDefinition artifact): self-referential — this is the first IndexDefinition artifact
   and the first entry in its own index. All subsequently created IndexDefinition artifacts have their key_type uniqueness
   enforced by this index.
3. **`all_index_definitions`** (IndexDefinition artifact): the global index for IndexDefinition artifacts; enforced by
   step 2.
4. **TypeDefinition type** (TypeDefinition + TypeVersionDefinition artifacts): declares `type_name_unique` and
   `all_types` indexes on itself.
5. **`type_name_unique`** and **`all_types`** (IndexDefinition artifacts): created for the TypeDefinition type's declared
   indexes; key_type uniqueness enforced by step 2.
6. **TypeVersionDefinition type** (TypeDefinition + TypeVersionDefinition artifacts): declares `type_versions_by_type`
   index on itself.
7. **`type_versions_by_type`** (IndexDefinition artifact): created for the TypeVersionDefinition type's declared index;
   key_type uniqueness enforced by step 2.
8. **ReferenceDefinition type** (TypeDefinition + TypeVersionDefinition artifacts): declares reference indexes on itself.
9. **`reference_key_type_unique`**, **`references_by_target_type`**, and **`all_reference_definitions`** (IndexDefinition
   artifacts): created for the ReferenceDefinition type's declared indexes; key_type uniqueness enforced by step 2.
10. **Built-in ReferenceDefinition artifacts**: materialize reference declarations for built-in types (for example,
    `TypeVersionDefinition.type_id` -> `TypeDefinition`, `TypeDefinition.current_version_id` -> `TypeVersionDefinition`).
11. **All derived index entries**: populate every bootstrap index with entries for all artifacts created above.
12. **Atomic commit**: the entire genesis state is committed as a single transaction to the canonical branch.

Each built-in type has exactly one TypeVersionDefinition at genesis, so `previous_version_id` and `next_version_id` are
both unset. Subsequent versions registered via the standard RegisterTypeVersion path will link to the genesis version.

After the genesis commit, the system is self-describing and all subsequent operations (including user type registrations)
use the standard RegisterTypeVersion code path. The genesis commit is idempotent — if the canonical branch already
contains the genesis state, initialization is a no-op.

#### Type metadata via custom options

Type metadata is expressed as custom options on the proto message and stored within the TypeVersionDefinition's descriptor
set. The artifact layer does not denormalize metadata onto the TypeVersionDefinition message; it reads metadata from the
descriptor set at runtime. An artifact type's metadata includes:
1. Indexes: a repeated MessageOption (`indexes`) listing index definitions. See Index options schema.
2. Actions: a MessageOption defining a dictionary of actions available on artifacts of this type. Stored as metadata at
   registration; interpretation is an App Layer concern and is not consumed by the artifact layer at launch.
3. Viewer: a MessageOption defining the default viewer endpoint. Same as Actions — stored but not interpreted at launch.
4. Custom Instruction: a MessageOption defining LLM instructions for the type.
5. Descriptions: MessageOption and FieldOption strings for human- and LLM-readable descriptions.
6. Referential integrity: a FieldOption (`references`) declaring artifact-to-artifact references.
7. A FieldOption for single-field indexes may be added later.

#### Protocol buffers as the type definition

Protocol buffers are the canonical representation for types. Each TypeVersionDefinition artifact stores a compiled
google.protobuf.FileDescriptorSet and the original .proto source. The descriptor set includes the defining .proto and
all transitive imports (excluding system protos, which are injected at load time — see Custom options as metadata). The
descriptor set is authoritative for parsing, validation, and index derivation. The registry must accept new .proto
definitions at runtime and compile them into a descriptor set on registration. The original .proto source is stored
alongside the descriptor set for inspection and re-compilation, but the descriptor set is the required runtime artifact.

The type_name on the parent TypeDefinition is the fully-qualified proto message name (for example,
`mypackage.DataFrameArtifact` or simply `DataFrameArtifact` if no package is declared). The registry uses type_name to
locate the artifact payload message within the descriptor set. This means the external API identifier for a type is the
same as the proto message name, ensuring a single unambiguous mapping between types and their schemas.

Standard protobuf validation for registration is: compile with protoc, reject parse/descriptor errors
(`PROTO_COMPILATION_FAILURE`), and only accept proto3 syntax for launch. Runtime compilation should enforce resource limits (maximum input file size, compilation
timeout, nesting depth) to prevent resource exhaustion from malformed or adversarial schemas. Custom options are
supported (see below) and must be retained at runtime so that metadata is available via descriptors.

#### Custom options as metadata

Type metadata is defined using protobuf custom options (extensions). In proto3, extensions are permitted only for custom
options, which aligns with our needs.
1. Message options: indexes, actions, viewer endpoint, LLM instructions, message descriptions.
2. Field options: referential integrity (`references`) and field descriptions.
3. Field options (optional): syntactic sugar for single-field indexes.
4. Virtual fields (planned): a MessageOption for declaring computed fields; the expression AST is shared with index
   predicates and is a near-term design decision (see Post-MVP notes).

Custom options must use runtime retention (not source-only retention) so they appear in the descriptor set. The registry
loads the descriptor set with the option extensions so the artifact layer can read metadata deterministically.

The artifact system's own proto definitions (IndexDefinition, ReferenceDefinition, ReferenceOption, OrderDefinition,
the expression AST (post-MVP), the `indexes` and `references` message extensions, and other system messages) are
injected as well-known imports at load time rather than stored in each
TypeVersionDefinition's descriptor set. This keeps descriptor sets smaller and ensures that system proto changes do not
require re-registering all existing types. During compilation, the registry provides these system protos as available
imports alongside the standard google.protobuf imports.

#### Index options schema (example)

The type-level option indexes is a repeated list of IndexDefinition objects. Each `option (indexes) = { ... }` entry
defines one index for the enclosing message type.

OrderDefinition.direction is required; the registry rejects index definitions with ORDER_BY_UNSPECIFIED
(`INVALID_INDEX_DEFINITION`).

Index where clauses are post-MVP; see Post-MVP notes for candidate expression AST and validation rules.

ReferenceOption is valid only on `uint64`, `optional uint64`, or `repeated uint64` fields. `on_delete` is required, and
`SET_NULL` is only valid on `optional` or `repeated` fields. Each reference requires exactly one covering index with the
reference field as the sole key (no where clause in the MVP); the registry rejects missing or ambiguous coverage
(`INVALID_REFERENCE_DECLARATION`).
Repeated reference fields must not contain duplicate values. Post-MVP may allow a single `IS_SET` predicate for optional
fields.

```proto
syntax = "proto3";

import "google/protobuf/descriptor.proto";

message OrderDefinition {
  string field = 1;
  enum OrderBy {
    ORDER_BY_UNSPECIFIED = 0;
    ASCENDING = 1;
    DESCENDING = 2;
  }
  OrderBy direction = 2;
}

message IndexDefinition {
  option (indexes) = { key_type: "index_key_type_unique" key: ["key_type"] order: { field: "artifact_id" direction: ASCENDING } unique: true };
  option (indexes) = { key_type: "all_index_definitions" key: [] order: { field: "artifact_id" direction: ASCENDING } };

  string key_type = 1;
  repeated string key = 2;
  repeated OrderDefinition order = 3;
  optional WhereClause where = 4; // post-MVP (see Post-MVP notes)
  bool unique = 5;
}

extend google.protobuf.MessageOptions {
  repeated IndexDefinition indexes = 50002;
  optional string description = 50004;
}

message ReferenceOption {
  string target_type_name = 1;
  enum OnDelete {
    ON_DELETE_UNSPECIFIED = 0;
    RESTRICT = 1;
    CASCADE = 2;
    SET_NULL = 3;
  }
  OnDelete on_delete = 2;
}

extend google.protobuf.FieldOptions {
  optional ReferenceOption references = 50003;
  optional string description = 50005;
}

// Built-in types: TypeDefinition, TypeVersionDefinition, IndexDefinition, and
// ReferenceDefinition are stored as artifacts and use the same index mechanics
// as all other artifact types.

message TypeDefinition {
  option (indexes) = { key_type: "type_name_unique" key: ["type_name"] order: { field: "artifact_id" direction: ASCENDING } unique: true };
  option (indexes) = { key_type: "all_types" key: [] order: { field: "artifact_id" direction: ASCENDING } };

  string type_name = 1;
  optional uint64 current_version_id = 2;
  bool deny_create = 3;
  bool deny_update = 4;
  bool deny_delete = 5;
}

message TypeVersionDefinition {
  option (indexes) = { key_type: "type_versions_by_type" key: ["type_id"] order: { field: "artifact_id" direction: ASCENDING } };

  uint64 type_id = 1;
  google.protobuf.FileDescriptorSet descriptor_set = 2;
  string proto_source = 3;
  optional uint64 previous_version_id = 4;
  optional uint64 next_version_id = 5;
}

message ReferenceDefinition {
  option (indexes) = { key_type: "reference_key_type_unique" key: ["key_type"] order: { field: "artifact_id" direction: ASCENDING } unique: true };
  option (indexes) = { key_type: "references_by_target_type" key: ["target_type_name"] order: { field: "artifact_id" direction: ASCENDING } };
  option (indexes) = { key_type: "all_reference_definitions" key: [] order: { field: "artifact_id" direction: ASCENDING } };

  string key_type = 1;
  string target_type_name = 2;
  string referencing_type_name = 3;
  string field_name = 4;
  string covering_index_key_type = 5;
  ReferenceOption.OnDelete on_delete = 6;
}

// Example user-defined artifact type. The type_name for this type is "DataFrameArtifact"
// (the fully-qualified proto message name).

message DataFrameArtifact {
  option (indexes) = { key_type: "by_owner" key: ["created_by"] order: { field: "artifact_id" direction: ASCENDING } };
  option (indexes) = { key_type: "by_repo_created_by" key: ["repo_id", "created_by"] order: { field: "artifact_id" direction: ASCENDING } };

  uint64 created_by = 1 [(references) = { target_type_name: "User" on_delete: RESTRICT }];
  uint64 repo_id = 2;
  bytes dataframe = 3;
}
```

#### Index object schema (generated example)

The stored index payload is the full `Index_*` proto3 message containing both the key (as an `IndexKey_*` sub-message)
and the value (as an `IndexValue_*` sub-message with typed `repeated` fields forming parallel columnar arrays). Each
order field maps to a `repeated` field of the corresponding protobuf type, and all columns have the same length. The
`IndexKey_*` message is also used as the API input for `FetchIndex` requests. Key fields are always stored in the payload
because the index object path is a content-addressed hash of the encoded key bytes (see Index physical storage).

For the DataFrameArtifact example, the by_owner index uses key = created_by and the by_repo_created_by index uses
key = (repo_id, created_by). The generated schemas for those indexes are:

```proto
syntax = "proto3";

message IndexValue_DataFrameArtifact_by_owner {
  uint32 row_count = 1;
  repeated uint64 artifact_id = 2;
}

message IndexKey_DataFrameArtifact_by_owner {
  uint64 created_by = 1;
}

message Index_DataFrameArtifact_by_owner {
  IndexKey_DataFrameArtifact_by_owner key = 1;
  IndexValue_DataFrameArtifact_by_owner value = 2;
}

message IndexValue_DataFrameArtifact_by_repo_created_by {
  uint32 row_count = 1;
  repeated uint64 artifact_id = 2;
}

message IndexKey_DataFrameArtifact_by_repo_created_by {
  uint64 repo_id = 1;
  uint64 created_by = 2;
}

message Index_DataFrameArtifact_by_repo_created_by {
  IndexKey_DataFrameArtifact_by_repo_created_by key = 1;
  IndexValue_DataFrameArtifact_by_repo_created_by value = 2;
}
```

If an index has additional order fields, include one `repeated` typed column per order field in order. For example, an
index with `order: [{ field: "created_at" }, { field: "artifact_id" }]` on a message where
`created_at` is `int64` would generate:

```proto
message IndexValue_Example_by_time {
  uint32 row_count = 1;
  repeated int64 created_at = 2;
  repeated uint64 artifact_id = 3;
}
```

All `repeated` columns must have exactly `row_count` elements. The artifact layer validates `row_count` at write time as a
defensive check (since the artifact layer itself authors index objects, this guards against implementation bugs) and
rejects index payloads where column lengths differ or `row_count` does not match.

#### Field presence and indexing semantics

Index derivation uses protobuf field semantics:
1. Optional fields have explicit presence; if unset, the field is treated as missing for index keys/order fields and the
   artifact contributes no index entry for that index. When predicate-based indexing is added (post-MVP), missing fields
   evaluate to false in predicate clauses (no three-valued logic).
2. Implicit-presence scalar fields (proto3 without optional) have no presence; missing is indistinguishable from the
   default value. For any field used in index keys/order fields (and future predicates), prefer optional.
3. Message-typed fields always have presence; they can be indexed only if a specific scalar sub-field is referenced.
4. Repeated scalar fields generate one index entry per element value. When a repeated field is used as an index key
   field, the artifact appears in a separate index object for each distinct value in the repeated field. For example, if
   an artifact has `tags = [10, 30, 55]` and the index key includes `tags`, the artifact ID appears in the index objects
   for key=10, key=30, and key=55. When the repeated field changes, entries are removed from index objects for values no
   longer present and added to index objects for new values. This mechanism also supports future virtual/computed fields
   (for example, deterministic keyword extraction) where a single artifact maps to multiple index keys. Repeated message
   fields require a scalar sub-field to be referenced. For launch, at most one repeated field is permitted per index
   definition (across key and order fields combined) to avoid cartesian-product fan-out during writes and merges. The
   registry rejects index definitions that reference more than one repeated field (`INVALID_INDEX_DEFINITION`).
5. Map fields are not indexable for launch; model indexable map-like data as repeated entry messages.

#### Type identity and versioning

A logical type is represented by a TypeDefinition artifact identified by type_name (the fully-qualified proto message
name). Type versions are represented by TypeVersionDefinition artifacts, each linked to its parent TypeDefinition via the
type_id field. Type versions cannot be created, updated, or deleted through the public CRUD API once registered (the
TypeVersionDefinition type has `deny_create = true, deny_update = true, deny_delete = true`); any schema or metadata
change creates a new TypeVersionDefinition artifact via RegisterTypeVersion.

The TypeDefinition artifact contains a `current_version_id` field pointing to the default TypeVersionDefinition
artifact_id. RegisterTypeVersion automatically sets `current_version_id` to the newly created version, so it always
points to the most recently registered version for a type. If a CRUD request omits the version_id, the artifact layer
resolves to the current_version_id at request time and returns the resolved version artifact_id in the response for
reproducibility.

Versions are identified by their artifact_id (a uint64), not by a human-readable version string. The
`type_versions_by_type` index lists all version artifact_ids for a given type_id in creation order (ascending
artifact_id). Callers that need a specific non-current version must reference it by artifact_id, which is returned at
registration time.

Type versions form an explicit doubly-linked list per type via `previous_version_id` and `next_version_id` pointers on
TypeVersionDefinition. The first version for a type has `previous_version_id` unset; the most recent (tail) version has
`next_version_id` unset. When a new version is registered, the artifact layer sets the new version's
`previous_version_id` to the current tail and updates the tail's `next_version_id` to the new version's artifact_id.
This linked-list structure enforces explicit serialization of version registration: if two transactions concurrently
register versions for the same type, both attempt to update the same tail version's `next_version_id`, producing an
artifact payload conflict that is never auto-resolved (see Conflict retry policy). This forces concurrent registrations
to be retried sequentially, preventing ambiguous version ordering.

### Launch API surface (minimum)

The Artifact Layer exposes a minimal API surface for launch and uses gRPC for service-to-service communication. The
server hides Storage Layer branch and merge mechanics; callers interact with snapshots and transactions and receive
structured conflict responses at Artifact Layer transaction commit time. Client-to-app-layer transport remains
application-specific and will be decided per application.

#### Snapshot and transaction API

Operations:
1. CreateSnapshot(parent_id?) -> snapshot_id
2. CreateTransaction(parent_id?) -> transaction_id
3. CommitTransaction(transaction_id)
4. RollbackTransaction(transaction_id)

Semantics are defined in Consistency and transaction semantics. All CRUD and index fetch operations accept an optional
snapshot_id or transaction_id to scope reads and writes. Writes issued without a transaction context are wrapped in an
implicit transaction (see Implicit transactions).

#### Artifact CRUD API

Operations (gRPC service-to-service; client-to-app transport is application-specific):
1. CreateArtifact(type_name, version_id?, payload, transaction_id?)
2. GetArtifact(artifact_id, snapshot_id | transaction_id?)
3. BatchGetArtifacts(artifact_ids, snapshot_id | transaction_id?)
4. UpdateArtifact(artifact_id, type_name, version_id?, payload, transaction_id?)
5. DeleteArtifact(artifact_id, transaction_id?)

Request/response expectations:
1. artifact_id is allocated by the artifact layer via a separate ID service and returned in Create responses.
1. version_id is an optional uint64 (TypeVersionDefinition artifact_id) for Create/Update; resolution behavior is defined
   in Type identity and versioning. If omitted, the artifact layer resolves to the TypeDefinition's current_version_id.
1. CreateArtifact is rejected (`MUTATION_DENIED`) if the TypeDefinition's `deny_create` flag is true; UpdateArtifact is
   rejected if `deny_update` is true; DeleteArtifact is rejected if `deny_delete` is true. See CRUD write error response
   for all write-time validation error categories and their gRPC status codes.
1. Create/Update validate referential integrity for reference fields (see CRUD write error response for violation
   categories); deletes may be rejected (`REFERENCE_DELETE_RESTRICTED` for RESTRICT) or may cascade or nullify
   references depending on declared `on_delete` behavior.
1. Reads return payload bytes plus type_name and version_id (the resolved TypeVersionDefinition artifact_id).
1. BatchGetArtifacts returns results in the same order as the input IDs using explicit per-id results (for example, a
   `oneof { artifact, not_found }` wrapper) so missing or tombstoned artifacts preserve positional correlation.
   BatchGetArtifacts does not follow references; see App Layer guidance for limitations.
1. Create/Update/Delete return the resolved version_id.
1. Delete and tombstone behavior is defined in Delete semantics and retention.
1. Conflicts are surfaced at Artifact Layer transaction commit time (CommitTransaction), not on individual write calls.
   For implicit transactions, the conflict response is returned from the write call itself.

Conflict/error behavior: conflict types, response payloads, and auto-resolution eligibility are defined in the Conflict
retry policy (MVP). The conflict response uses conflict_type values INDEX_CONFLICT, PAYLOAD_CONFLICT, and
REFERENTIAL_INTEGRITY_VIOLATION.

#### CRUD write error response

Write-time validation failures on CreateArtifact, UpdateArtifact, and DeleteArtifact return gRPC status
`INVALID_ARGUMENT` with an `ArtifactWriteError` detail message. The error contains one or more
`ArtifactWriteViolation` entries so callers can diagnose and fix all issues in a single pass. All applicable
validations run to completion and their violations are collected together before the error is returned (the write is
not applied). For implicit transactions the error is returned from the write call itself; for explicit transactions the
error is returned from the individual write call within the transaction (not deferred to CommitTransaction), and the
sub-branch for that write is abandoned without merging into the transaction branch (the transaction remains open).

Each violation includes:
1. category: one of the `ArtifactWriteViolation.Category` values below.
2. description: human-readable string explaining the specific failure (for example, "CreateArtifact denied: type
   'IndexDefinition' has deny_create = true").
3. subject: a short identifier for the entity involved — a type_name (for example, "type: IndexDefinition"), a field
   name (for example, "field: created_by"), an artifact_id (for example, "artifact_id: 42"), or an index key_type (for
   example, "index: by_owner"). The subject is a simple string to allow the format to evolve without breaking consumers.

Violation categories:

1. `MUTATION_DENIED` — the write is rejected because the TypeDefinition's mutation-restriction flag is set.
   `deny_create = true` rejects CreateArtifact, `deny_update = true` rejects UpdateArtifact, `deny_delete = true`
   rejects DeleteArtifact. The description identifies which flag triggered the rejection and the type_name. The subject
   is the type_name. This is the only violation returned when triggered (the flag check runs before payload validation).
2. `PAYLOAD_VALIDATION_FAILURE` — the payload fails proto3 structural validation against the message identified by
   type_name in the resolved TypeVersionDefinition's descriptor set. Covers: wire-format parse errors, unknown fields
   that violate the schema, type mismatches on known fields, and any other structural check that the proto3 runtime
   rejects. The description includes the proto3 validation error text. The subject is the type_name. This category
   does not cover semantic or business validation (out of scope for launch).
3. `EMPTY_PAYLOAD` — CreateArtifact or UpdateArtifact received a zero-length payload. Empty payloads are reserved for
   tombstones (see Delete semantics and retention). The description is "empty payload: zero-length payloads are
   reserved for tombstones". The subject is the type_name. The empty-payload check runs before proto3 structural
   validation; if the payload is zero-length, this is the only violation returned.
4. `NAN_IN_INDEXED_FIELD` — a float or double field used as an index key contains a NaN value. NaN values are
   non-deterministic for encoding and incomparable for ordering, so they are rejected during index derivation (see Index
   key encoding). Because artifact writes and index derivation occur within the same transaction, a NaN in any indexed
   float field rejects the entire write. The description identifies the field name and the index key_type. The subject
   is the field name (for example, "field: score"). Multiple violations may be returned if NaN values appear in
   multiple indexed fields.
5. `NON_MINIMAL_VARINT` — a string or bytes field used as an index key produced a non-minimal varint length prefix
   during index key encoding. Varint length prefixes must use minimal encoding (the shortest base-128 representation
   with no leading zero groups) for deterministic hashing (see Index key encoding). The description identifies the field
   name and the index key_type. The subject is the field name. In practice this category guards against implementation
   bugs in the encoding layer rather than user input errors, since the artifact layer itself encodes varints; it is
   included for completeness and defensive correctness.
6. `REFERENCE_TARGET_NOT_FOUND` — a reference field points to an artifact_id that does not exist. The description
   identifies the reference field, the target artifact_id, and the expected target_type_name. The subject is the field
   name.
7. `REFERENCE_TARGET_TOMBSTONED` — a reference field points to an artifact_id that exists but is tombstoned (logically
   deleted). The description identifies the reference field, the target artifact_id, and the expected target_type_name.
   The subject is the field name.
8. `REFERENCE_TARGET_WRONG_TYPE` — a reference field points to an artifact_id that exists and is live, but its
   type_name does not match the declared `target_type_name`. The description identifies the reference field, the target
   artifact_id, the expected target_type_name, and the actual type_name. The subject is the field name.
9. `REFERENCE_DUPLICATE_VALUE` — a repeated reference field contains duplicate artifact_id values. The description
   identifies the reference field and the duplicated artifact_id(s). The subject is the field name.
10. `REFERENCE_DELETE_RESTRICTED` — a DeleteArtifact is rejected because one or more referencing artifacts exist with
    `on_delete = RESTRICT` and are not scheduled for deletion in the current transaction. The description identifies the
    ReferenceDefinition key_type and the referencing artifact_ids. The subject is the ReferenceDefinition key_type (for
    example, "reference: DataFrameArtifact.created_by").

**Validation order and short-circuiting**

Validations are applied in the following order. A category marked "short-circuit" means that if any violation of that
category is produced, subsequent validation phases are skipped (because later phases depend on earlier ones succeeding).

1. `MUTATION_DENIED` — checked first. Short-circuit: if denied, no further validation is performed.
2. `EMPTY_PAYLOAD` — checked before parsing. Short-circuit: if empty, no further validation is performed. (Applies to
   Create/Update only; Delete payloads are always empty by definition.)
3. `PAYLOAD_VALIDATION_FAILURE` — proto3 structural parse/validation. Short-circuit: if the payload cannot be parsed,
   field-level validations (referential integrity, index derivation) cannot proceed.
4. `NAN_IN_INDEXED_FIELD`, `NON_MINIMAL_VARINT` — index derivation checks. These run for all indexed fields and all
   violations are collected.
5. `REFERENCE_TARGET_NOT_FOUND`, `REFERENCE_TARGET_TOMBSTONED`, `REFERENCE_TARGET_WRONG_TYPE`,
   `REFERENCE_DUPLICATE_VALUE` — referential integrity checks on Create/Update. All reference fields are validated and
   all violations are collected.
6. `REFERENCE_DELETE_RESTRICTED` — referential integrity check on Delete. All RESTRICT references are checked and all
   violations are collected.

Phases 4 and 5 run independently and their violations are collected together. Within each phase, all applicable checks
run to completion.

```proto
message ArtifactWriteError {
  repeated ArtifactWriteViolation violations = 1;
}

message ArtifactWriteViolation {
  enum Category {
    CATEGORY_UNSPECIFIED = 0;
    MUTATION_DENIED = 1;
    PAYLOAD_VALIDATION_FAILURE = 2;
    EMPTY_PAYLOAD = 3;
    NAN_IN_INDEXED_FIELD = 4;
    NON_MINIMAL_VARINT = 5;
    REFERENCE_TARGET_NOT_FOUND = 6;
    REFERENCE_TARGET_TOMBSTONED = 7;
    REFERENCE_TARGET_WRONG_TYPE = 8;
    REFERENCE_DUPLICATE_VALUE = 9;
    REFERENCE_DELETE_RESTRICTED = 10;
  }
  Category category = 1;
  string description = 2;
  string subject = 3;
}
```

#### Index fetch API

Operation:
1. FetchIndex(key_type, key, snapshot_id | transaction_id?)

Inputs:
1. key_type identifies the index definition.
1. key must include the complete set of key fields; partial keys are rejected.
1. An optional snapshot_id or transaction_id scopes the read (see Index fetch behavior).

Response:
1. Returns the index object stored for (key_type, encoded key values) in deterministic order.
1. Response shape and field semantics are defined in Index fetch behavior (MVP), including the generated index message.

#### Type registry API

The registry supports registering and resolving type versions. All registry operations create or modify TypeDefinition,
TypeVersionDefinition, IndexDefinition, and ReferenceDefinition artifacts via the standard artifact storage path, but
enforce additional validation rules that the generic CRUD API does not.

1. RegisterTypeVersion(type_name, proto_source, deny_create?, deny_update?, deny_delete?) -> version_id
1. GetTypeVersion(version_id) -> TypeVersionDefinition
1. ListTypeVersions(type_name) -> repeated version_id
1. ResolveTypeVersion(type_name) -> version_id (current)

**RegisterTypeVersion** compiles the .proto source into a FileDescriptorSet (injecting system protos as well-known
imports), locates the message identified by type_name in the descriptor set, and validates the schema. It then:
1. Looks up the TypeDefinition via the `type_name_unique` index. If not found, creates a new TypeDefinition artifact
   (`type_name`, `current_version_id` set to the new TypeVersionDefinition's artifact_id — see step 6, `deny_create`,
   `deny_update`, and `deny_delete` set to the caller-provided values or false if omitted). If the TypeDefinition
   already exists and the caller provides
   `deny_create`, `deny_update`, or `deny_delete` values, the registry enforces the tighten-only rule: each flag may
   be changed from false to true but never from true
   to false. A request that attempts to loosen a restriction (true to false) is rejected (`TIGHTEN_ONLY_VIOLATION`). If the caller omits the
   flags on a subsequent registration, the existing values are preserved unchanged.
2. If a prior TypeVersionDefinition exists for this type (checked via `type_versions_by_type` index), identifies the
   tail version (the version whose `next_version_id` is unset) and validates schema compatibility against it
   (`SCHEMA_INCOMPATIBILITY` for field/type changes; see schema compatibility rules below).
3. Extracts index declarations from the message's custom options. For each key_type, checks the `index_key_type_unique`
   index: if an IndexDefinition already exists, validates compatibility (`INDEX_INCOMPATIBILITY`); if not, validates
   the new definition (`INVALID_INDEX_DEFINITION`) and creates a new IndexDefinition artifact.
4. Extracts reference declarations from field options. For each reference, validates field type, `on_delete`,
   `target_type_name`, and presence of a covering index (reference field as sole key; no where clause in the MVP; post-MVP
   may allow `IS_SET`) (`INVALID_REFERENCE_DECLARATION`). For each reference, checks `reference_key_type_unique`: if a
   ReferenceDefinition already exists, validates compatibility (`REFERENCE_INCOMPATIBILITY`); if not, creates a new
   ReferenceDefinition artifact.
5. Creates a TypeVersionDefinition artifact (type_id = TypeDefinition artifact_id, descriptor_set, proto_source,
   `previous_version_id` = tail version's artifact_id if a prior version exists, `next_version_id` unset). If a tail
   version exists, updates the tail version's `next_version_id` to the new version's artifact_id. This update uses an
   internal bypass of the `deny_update` restriction (see Internal bypass of mutation restrictions). The update to the
   tail version is the mechanism that enforces serialization of concurrent registrations: if two transactions both
   attempt to set `next_version_id` on the same tail, the second to commit produces an artifact payload conflict (see
   Conflict retry policy).
6. Updates the TypeDefinition artifact's `current_version_id` to the new TypeVersionDefinition's artifact_id. This
   uses an internal bypass of the `deny_update` restriction (TypeDefinition has `deny_update = true`; see Internal
   bypass of mutation restrictions). For a new TypeDefinition (created in step 1), `current_version_id` is set as part
   of the initial creation. For an existing TypeDefinition, this is an update within the same transaction. Because the
   TypeDefinition is also updated when tightening mutation-restriction flags (step 1), and the tail
   TypeVersionDefinition is updated in step 5, concurrent registrations for the same type will produce payload
   conflicts on both the tail version and the TypeDefinition, reinforcing serialization.
7. Derives all index entries for the new artifacts and commits the Artifact Layer transaction atomically (merging the
   ephemeral branch into the canonical branch in the Storage Layer).
8. Returns the new TypeVersionDefinition artifact_id (the version_id).

Schema compatibility rules for launch (violations return the indicated category; see RegisterTypeVersion error response):
1. Existing fields must not be removed or have their type changed (`SCHEMA_INCOMPATIBILITY`). These rules apply
   recursively to nested message types: removing or changing a field in a nested message is a breaking change. Adding
   fields to a `oneof` is permitted; removing `oneof` fields or the `oneof` itself is a breaking change. Changing a
   field between `optional`, `required`, and `repeated` is a type change.
1. Existing index definitions must not be removed or modified (`INDEX_INCOMPATIBILITY`). A modification is any change
   that would alter what is indexed or the shape of the index payload: changes to key fields, order fields, or the
   unique flag. Such changes require a full index rebuild (backfill), which is post-launch. Predicate changes (post-MVP)
   would also require a rebuild.
1. Existing reference declarations must not be removed or modified (`REFERENCE_INCOMPATIBILITY`). A modification includes
   changes to target_type_name, the reference field, the covering index key_type, or the on_delete behavior. Such
   changes are post-launch.
1. New fields, new indexes, and new reference declarations may be added. New indexes apply only to writes after
   registration (no backfill in the MVP), so index completeness for historical data is not guaranteed.
1. Field number reassignment is rejected (`SCHEMA_INCOMPATIBILITY`).
1. Mutation-restriction flags (`deny_create`, `deny_update`, `deny_delete`) may only be tightened
   (`TIGHTEN_ONLY_VIOLATION`): a flag that is true on the existing TypeDefinition cannot be set to false. A registration
   that omits the flags preserves the existing values. See step 1 of RegisterTypeVersion above.

**RegisterTypeVersion error response**

RegisterTypeVersion validation failures return gRPC status `INVALID_ARGUMENT` with a `RegisterTypeVersionError` detail
message. The error contains one or more `TypeRegistrationViolation` entries so callers can fix all issues in a single
pass. If proto compilation itself fails, that is the only violation returned (subsequent validations depend on a
successfully compiled descriptor set). All other validation checks run to completion and their violations are collected
together.

Each violation includes:
1. category: one of the `TypeRegistrationViolation.Category` values below.
2. description: human-readable string explaining the specific failure (for example, "field 'created_by' changed type from
   uint64 to string").
3. subject: a short identifier for the entity involved — a field name or number (for example, "field: created_by" or
   "field_number: 3"), an index key_type (for example, "index: by_owner"), a reference key_type (for example,
   "reference: DataFrameArtifact.created_by"), or a flag name (for example, "flag: deny_delete"). The subject is a
   simple string to allow the format to evolve without breaking consumers.

Violation categories:

1. `PROTO_COMPILATION_FAILURE` — the .proto source failed to compile, uses non-proto3 syntax, or the type_name does not
   resolve to a message in the resulting descriptor set. This category is fatal: if present, no further validations are
   performed and this is the only violation in the response.
2. `SCHEMA_INCOMPATIBILITY` — the new schema is incompatible with the prior type version. Covers: existing field removed,
   existing field type changed (including changes between `optional`/`required`/`repeated`), nested message field
   removed or type-changed, `oneof` removed or field removed from a `oneof`, and field number reassignment (a field
   number reused for a different field name).
3. `INVALID_INDEX_DEFINITION` — a newly declared index definition is structurally invalid. Covers: `ORDER_BY_UNSPECIFIED`
   on an order field, more than one repeated field referenced across key and order fields, and other structural violations
   of the IndexDefinition schema.
4. `INDEX_INCOMPATIBILITY` — an existing index definition was removed or modified. A modification is any change to key
   fields, order fields, or the unique flag. Such changes require an index rebuild (post-launch).
5. `INVALID_REFERENCE_DECLARATION` — a newly declared reference is structurally invalid. Covers: `references` option on a
   non-`uint64`/`optional uint64`/`repeated uint64` field, `ON_DELETE_UNSPECIFIED`, `SET_NULL` on an implicit-presence
   scalar, `target_type_name` that does not resolve to an existing TypeDefinition, and missing or ambiguous covering
   index (no index with the reference field as the sole key, or multiple such indexes).
6. `REFERENCE_INCOMPATIBILITY` — an existing reference declaration was removed or modified. A modification includes
   changes to `target_type_name`, the reference field, the covering index `key_type`, or the `on_delete` behavior.
7. `TIGHTEN_ONLY_VIOLATION` — a mutation-restriction flag (`deny_create`, `deny_update`, or `deny_delete`) was set to
   false when the existing TypeDefinition has it set to true.

```proto
message RegisterTypeVersionError {
  repeated TypeRegistrationViolation violations = 1;
}

message TypeRegistrationViolation {
  enum Category {
    CATEGORY_UNSPECIFIED = 0;
    PROTO_COMPILATION_FAILURE = 1;
    SCHEMA_INCOMPATIBILITY = 2;
    INVALID_INDEX_DEFINITION = 3;
    INDEX_INCOMPATIBILITY = 4;
    INVALID_REFERENCE_DECLARATION = 5;
    REFERENCE_INCOMPATIBILITY = 6;
    TIGHTEN_ONLY_VIOLATION = 7;
  }
  Category category = 1;
  string description = 2;
  string subject = 3;
}
```

**ListTypeVersions** fetches the `type_versions_by_type` index for the TypeDefinition's artifact_id, returning version
artifact_ids in creation order.

RegisterTypeVersion automatically sets `current_version_id` to the newly created version (see step 6 of
RegisterTypeVersion). There is no separate operation to change the current version pointer. The `current_version_id`
always points to the most recently registered version for a type. Callers that need a specific non-current version
reference it by artifact_id (version_id) on CRUD calls. Type registration is a low-concurrency administrative operation;
contention is not expected.

## App Layer

The App Layer applies domain-specific rules, permissions, and workflows on top of the Artifact Layer. Its design is out
of scope for this document and will be covered in a separate PRD. An example App Layer service is an Artifact Viewer.

## Post-MVP notes

### Index where clauses and expression AST

Index where clauses are deferred from the MVP. When introduced, they gate index entry derivation only; FetchIndex never
re-evaluates predicates and returns the stored index state.

We will explore an AST structure that can handle both complex where clauses and virtual/computed fields. The scratch below
is a candidate shape and will be refined.

**Candidate semantics**
1. The predicate tree is recursive: leaf nodes are binary (==, !=, >, <, >=, <=) or unary (IS_SET) predicates; compound
   nodes are And or Or combinators that contain child clauses.
2. Binary clauses require a field as LHS and a field or constant as RHS.
3. IS_SET checks whether an `optional` field has explicit presence and is the preferred way to gate an index on
   presence. The != (NE) binary operator can also serve as a presence gate for `optional` fields (since missing fields
   evaluate to false for any comparison), but IS_SET is unambiguous and does not depend on choosing a sentinel RHS value.
4. If multiple conditions are needed, wrap them in an And or Or clause; there is no implicit conjunction.

**Validation (candidate)**
1. WhereClause must set exactly one variant of the `clause` oneof; the op field is required in leaf variants and
   OP_UNSPECIFIED is rejected.
2. For BinaryWhereClause, the LHS must be a field and the RHS must be set (field or constant).
3. For UnaryWhereClause, IS_SET is valid only on fields with explicit presence (`optional`); reject IS_SET on
   implicit-presence scalars.
4. AndWhereClause and OrWhereClause must contain at least two child clauses; reject empty or single-element compounds
   (use the child directly instead).
5. Enforce a maximum nesting depth to prevent unbounded recursion.

**Proto scratch (candidate)**
```proto
message Constant {
  oneof value {
    string string_value = 1;
    int64 int64_value = 2;
    uint64 uint64_value = 3;
    bool bool_value = 4;
    double double_value = 5;
  }
}

message BinaryWhereClause {
  string lhs = 1;
  enum Op {
    OP_UNSPECIFIED = 0;
    GT = 1;
    LT = 2;
    LTE = 3;
    GTE = 4;
    EQ = 5;
    NE = 6;
  }
  Op op = 2;
  oneof rhs {
    string rhs_field = 3;
    Constant rhs_value = 4;
  }
}

message UnaryWhereClause {
  string field = 1;
  enum Op {
    OP_UNSPECIFIED = 0;
    IS_SET = 1;
  }
  Op op = 2;
}

message AndWhereClause {
  repeated WhereClause clauses = 1;
}

message OrWhereClause {
  repeated WhereClause clauses = 1;
}

message WhereClause {
  oneof clause {
    BinaryWhereClause binary = 1;
    UnaryWhereClause unary = 2;
    AndWhereClause and = 3;
    OrWhereClause or = 4;
  }
}

message IndexDefinition {
  string key_type = 1;
  repeated string key = 2;
  repeated OrderDefinition order = 3;
  optional WhereClause where = 4;
  bool unique = 5;
}
```

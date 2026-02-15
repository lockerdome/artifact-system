# Layered Artifact System

## Launch Scope (MVP)

### In scope
1. Artifact CRUD with append-only writes and tombstone-based deletes (see Delete semantics and retention).
1. Artifact payloads are structured by registered protobuf schemas, with fields available for indexing; applications may
   still include opaque sections (for example, raw bytes or text/JSON) inside protobuf envelopes they define.
1. Test-and-set updates via storage version IDs (see Artifact contract); conflict ownership is defined in the Conflict
   retry policy.
1. Deterministic index derivation and merges for unique and non-unique indexes; launch uses inline index storage and
   preserves metadata for future sharded layouts.
1. Referential integrity with field-level reference options, write-time validation, and delete-time enforcement
   (restrict/cascade/set-null).
1. Type registry with runtime registration of proto3 schemas, descriptor sets as canonical runtime artifacts, and
   retained custom options including LLM instruction/description annotations at message and field levels. Types, type
   versions, index definitions, and reference definitions are first-class artifacts with their own indexes (see Built-in
   types and bootstrapping).
1. Type version resolution via an optional current pointer on TypeDefinition (see Type identity and versioning).
1. Artifact server API hides branches and transaction mechanics; callers only see conflicts and resolved storage version
   IDs.

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
1. Artifact writes use test-and-set semantics (see Artifact contract) to prevent lost updates across retries.

**Conflict response payload**
When retries are aborted or exhausted, return a structured conflict response containing:
1. conflict_type: one of {INDEX_CONFLICT, PAYLOAD_CONFLICT, VERSION_MISMATCH, REFERENTIAL_INTEGRITY_VIOLATION}.
1. retryable: boolean indicating whether the server would retry (false when exhausted or payload conflict).
1. attempts: number of attempts performed.
1. artifacts/indexes involved: artifact_id for payload conflicts; index key (key_type + encoded key) for index conflicts;
   artifact_id for version mismatches; for referential integrity violations include the target artifact_id, the reference
   key_type, and the referencing artifact_ids.
1. version_ids: base, ours, theirs storage version IDs when available; for VERSION_MISMATCH include expected_version_id
   and current_version_id.

**Exhaustion behavior**
1. If the retry limit is reached while resolving index conflicts, return the conflict response with retryable=false.
1. Callers may re-read state, resolve conflicts at the application level, and retry the operation with updated expected version IDs.

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
1. float: IEEE 754 binary32, little-endian; double: IEEE 754 binary64, little-endian. NaN values are rejected and -0 is
   normalized to +0 during index derivation. Because artifact writes and index derivation occur within the same
   transaction, a NaN in an indexed float field rejects the entire write (artifact payload and all derived indexes).
1. string: UTF-8 bytes prefixed by an unsigned varint length (base-128, LSB-first, minimal encoding).
1. bytes: raw bytes prefixed by an unsigned varint length (base-128, LSB-first, minimal encoding).

Varint length prefixes must use minimal encoding: the shortest base-128 representation with no leading zero groups. This
is required for deterministic encoding; non-minimal varints must be rejected during index derivation.

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
1. The artifact layer locates the index object for (key_type, encoded key values) and reads its current state from the
   canonical branch head by default. An optional version specifier (LakeFS commit ID) may be provided to read from a
   specific point in time, supporting MVCC and consistent reads across multiple fetch calls.
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

The response may also include the index object’s storage version ID for debugging or consistency checks.

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

Artifacts are stored as opaque payloads defined by Types. Writes use test-and-set via storage version IDs; conflict
ownership (payload vs. index) is defined in the Conflict retry policy. In the future, we can add application-defined
merge logic per artifact type.

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

On create/update, the artifact layer checks the TypeDefinition's `immutable` flag; if true, UpdateArtifact and
DeleteArtifact are rejected. The artifact layer validates the payload using standard proto3 structural validation against
the message identified by type_name in the TypeVersionDefinition's descriptor set, and derives index entries from the
index declarations in the descriptor set's custom options. Artifact validation is limited to proto3 structural checks
and referential integrity (see below); semantic or business validation is out of scope for launch. Responses return the
resolved version_id (the
TypeVersionDefinition artifact_id) and the storage version ID so callers can perform safe updates. Updates and deletes
require an expected storage version ID to avoid lost updates. Reads return payload bytes plus type_name, version_id, and
storage version ID. Partial updates are out of scope for launch.

The artifact layer stores payload bytes as provided and does not reserialize them. This preserves unknown fields and
avoids any reliance on canonical protobuf binary encodings. Text/JSON formats are not used for storage.

#### Referential integrity (MVP)

Referential integrity is expressed via field-level options on `uint64` fields that reference other artifacts by
artifact_id. Each reference declaration is materialized as a ReferenceDefinition artifact at registration time (see
Built-in types and bootstrapping). The descriptor set options are the source of truth; ReferenceDefinition artifacts are
derived materializations for reverse lookup and enforcement.

**Declaration and registration rules**
1. The references option is valid only on `uint64`, `optional uint64`, or `repeated uint64` fields. The registry rejects
   references on any other field type.
1. `on_delete` is required; `ON_DELETE_UNSPECIFIED` is rejected.
1. `SET_NULL` is valid on `optional` or `repeated` fields; it is rejected on implicit-presence scalars.
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
   verifies it exists, is not tombstoned, and its type_name matches `target_type_name`.
1. If a reference field is unset (optional), it is treated as null and does not require validation.
1. For repeated reference fields, each value is validated and the list must not contain duplicates; duplicate values are
   rejected on Create/Update.
1. References are enforced on create/update; existing stored artifacts are not revalidated until they are updated.

**Delete-time enforcement**
1. On Delete, the artifact layer resolves the artifact's type_name and fetches all ReferenceDefinition artifacts from the
   `references_by_target_type` index for that type.
1. For each ReferenceDefinition, it fetches the covering index with key = deleted artifact_id to list referencing
   artifacts.
1. Enforcement by `on_delete`:
   1. **RESTRICT**: reject the delete if any referencing artifacts exist that are not already scheduled for delete in the
      current transaction.
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

Deletes are logical tombstones. A delete writes a new version at the same artifact key with an empty payload, advances
the storage version ID, and returns that version to the caller.

1. Empty payloads are reserved for tombstones. The artifact layer rejects zero-length payloads for live artifacts; if a
   type needs to represent an "empty" value, include a sentinel field or wrap it in an envelope message.
1. Index handling: deleting an artifact removes all derived index entries for that artifact ID (as if the artifact no
   longer qualifies for any index entries). Unique indexes free the slot. If an index becomes empty, it is tombstoned by
   storing an index object with an empty value portion (row_count = 0 and no value entries) so merges can detect
   deletions.
1. Referential integrity: deletes may be rejected (RESTRICT), cascade, or nullify references based on reference field
   options; enforcement occurs in the same internal transaction (see Referential integrity).
1. Reads treat tombstones as not found for standard Get. There is no public restore or audit API in the MVP; to
   "undelete" a caller writes a new payload with the tombstone's expected version ID, creating a new version in history.
1. Retention: history is preserved as LakeFS commits but is subject to LakeFS GC/vacuum. To guarantee audit/restore
windows, pin commits/tags or configure retention accordingly. Hard delete/purge is out of scope for launch.

#### Consistency and transaction semantics

The Artifact Layer presents a single canonical branch to callers; branch and transaction mechanics are internal. The
internal strategy is branch-per-write: each Create/Update/Delete creates an ephemeral branch from the canonical branch
head, commits artifact and index changes to that branch, and merges back into the canonical branch. LakeFS merges operate
on metadata (not data copies), so merge operations are fast even at scale.

1. Each Create/Update/Delete runs as an internal transaction (ephemeral branch + merge) and is merged before the call
   returns success.
1. Delete operations that cascade or nullify references perform all dependent writes in the same internal transaction.
1. Read-after-write: once a write call succeeds, subsequent reads of that artifact key return the committed version and its storage version ID.
1. Consistency is per artifact key plus its derived index updates; there are no multi-key or cross-artifact atomic transactions in the MVP.
1. Concurrent writes may diverge internally; conflict ownership is defined in the Conflict retry policy.
1. If a write fails mid-transaction (for example, index derivation rejects a NaN float key), the ephemeral branch is
   abandoned without merging. There is no rollback; the branch simply never reaches the canonical branch. Abandoned
   branches are cleaned up by the LakeFS Spark GC service, which removes branches that are not recent and not referenced
   by tags or branch heads.
1. Unique index enforcement is described in Index definition and Index merge semantics.

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
   TypeVersionDefinition. Also carries an `immutable` flag: when true, UpdateArtifact and DeleteArtifact are rejected for
   artifacts of this type (Create is still permitted through the registry API).
2. **TypeVersionDefinition**: represents a specific version of a type. Contains the type_id (artifact_id of the parent
   TypeDefinition), the compiled FileDescriptorSet, and the original .proto source. Multiple TypeVersionDefinition
   artifacts may exist for a single TypeDefinition. TypeVersionDefinition is immutable (its TypeDefinition has
   `immutable = true`); once registered, a version cannot be modified or deleted. Metadata such as index definitions,
   actions, viewer endpoints, and LLM instructions are attached as custom options within the descriptor set and are not
   denormalized onto the TypeVersionDefinition message (see Custom options as metadata).
3. **IndexDefinition**: represents a single index. Contains the key_type, key fields, order fields, and unique flag.
   Where clauses are post-MVP. IndexDefinition is immutable. Each IndexDefinition artifact receives an artifact_id that
   is used as the key_prefix in index storage paths (see Index physical storage).
4. **ReferenceDefinition**: represents a single referential integrity declaration. Contains the referencing type, target
   type, reference field, covering index key_type, and on_delete behavior. key_type is a globally unique identifier;
   the recommended format is `{referencing_type_name}.{field_name}`. The unique key_type is used by the registry to
   detect duplicates across type versions without scanning all reference definitions. ReferenceDefinition is immutable
   and is created during RegisterTypeVersion when reference field options are detected.

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

Standard protobuf validation for registration is: compile with protoc, reject parse/descriptor errors, and only accept
proto3 syntax for launch. Runtime compilation should enforce resource limits (maximum input file size, compilation
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

OrderDefinition.direction is required; the registry rejects index definitions with ORDER_BY_UNSPECIFIED.

Index where clauses are post-MVP; see Post-MVP notes for candidate expression AST and validation rules.

ReferenceOption is valid only on `uint64`, `optional uint64`, or `repeated uint64` fields. `on_delete` is required, and
`SET_NULL` is only valid on `optional` or `repeated` fields. Each reference requires exactly one covering index with the
reference field as the sole key (no where clause in the MVP); the registry rejects missing or ambiguous coverage.
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
  bool immutable = 3;
}

message TypeVersionDefinition {
  option (indexes) = { key_type: "type_versions_by_type" key: ["type_id"] order: { field: "artifact_id" direction: ASCENDING } };

  uint64 type_id = 1;
  google.protobuf.FileDescriptorSet descriptor_set = 2;
  string proto_source = 3;
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
   registry rejects index definitions that reference more than one repeated field.
5. Map fields are not indexable for launch; model indexable map-like data as repeated entry messages.

#### Type identity and versioning

A logical type is represented by a TypeDefinition artifact identified by type_name (the fully-qualified proto message
name). Type versions are represented by TypeVersionDefinition artifacts, each linked to its parent TypeDefinition via the
type_id field. Type versions are immutable once registered; any schema or metadata change creates a new
TypeVersionDefinition artifact.

The TypeDefinition artifact contains an optional `current_version_id` field pointing to the default
TypeVersionDefinition artifact_id. If a CRUD request omits the version_id, the artifact layer resolves to the
current_version_id at request time and returns the resolved version artifact_id in the response for reproducibility. A
TypeDefinition with no current_version_id set is a valid state — versions can be used explicitly by callers that specify
a version_id, but no default resolution is available.

Versions are identified by their artifact_id (a uint64), not by a human-readable version string. The
`type_versions_by_type` index lists all version artifact_ids for a given type_id in creation order (ascending
artifact_id). Callers that need a specific non-current version must reference it by artifact_id, which is returned at
registration time.

### Launch API surface (minimum)

The Artifact Layer exposes a minimal API surface for launch and uses gRPC for service-to-service communication. The
server hides branches and transaction mechanics; callers only see resolved storage version IDs and structured
conflict responses. Client-to-app-layer transport remains application-specific and will be decided per application.

#### Artifact CRUD API

Operations (gRPC service-to-service; client-to-app transport is application-specific):
1. CreateArtifact(type_name, version_id?, payload)
2. GetArtifact(artifact_id)
3. BatchGetArtifacts(artifact_ids)
4. UpdateArtifact(artifact_id, expected_storage_version_id, type_name, version_id?, payload)
5. DeleteArtifact(artifact_id, expected_storage_version_id)

Request/response expectations:
1. artifact_id is allocated by the artifact layer via a separate ID service and returned in Create responses.
1. version_id is an optional uint64 (TypeVersionDefinition artifact_id) for Create/Update; resolution behavior is defined
   in Type identity and versioning. If omitted, the artifact layer resolves to the TypeDefinition's current_version_id.
1. Update/Delete require an expected storage version ID (test-and-set per Artifact contract); mismatches return a
   VERSION_MISMATCH conflict. Update/Delete are rejected if the TypeDefinition's `immutable` flag is true.
1. Create/Update validate referential integrity for reference fields; deletes may be rejected (RESTRICT) or may cascade
   or nullify references depending on declared `on_delete` behavior.
1. Reads return payload bytes plus type_name, version_id (the resolved TypeVersionDefinition artifact_id), and storage
   version ID.
1. BatchGetArtifacts returns results in the same order as the input IDs using explicit per-id results (for example, a
   `oneof { artifact, not_found }` wrapper) so missing or tombstoned artifacts preserve positional correlation.
1. Create/Update/Delete return the resolved version_id and the new storage version ID.
1. Delete and tombstone behavior is defined in Delete semantics and retention.

Conflict/error behavior: conflict types, response payloads, and auto-resolution eligibility are defined in the Conflict
retry policy (MVP). The conflict response uses conflict_type values INDEX_CONFLICT, PAYLOAD_CONFLICT, and
VERSION_MISMATCH, and REFERENTIAL_INTEGRITY_VIOLATION.

#### Index fetch API

Operation:
1. FetchIndex(key_type, key)

Inputs:
1. key_type identifies the index definition.
1. key must include the complete set of key fields; partial keys are rejected.

Response:
1. Returns the index object stored for (key_type, encoded key values) in deterministic order.
1. Response shape and field semantics are defined in Index fetch behavior (MVP), including the generated index message
   and optional storage version ID.

#### Type registry API

The registry supports registering and resolving type versions. All registry operations create or modify TypeDefinition,
TypeVersionDefinition, IndexDefinition, and ReferenceDefinition artifacts via the standard artifact storage path, but
enforce additional validation rules that the generic CRUD API does not.

1. RegisterTypeVersion(type_name, proto_source) -> version_id
1. GetTypeVersion(version_id) -> TypeVersionDefinition
1. ListTypeVersions(type_name) -> repeated version_id
1. SetCurrentTypeVersion(type_name, version_id, expected_version_id)
1. ResolveTypeVersion(type_name) -> version_id (current)

**RegisterTypeVersion** compiles the .proto source into a FileDescriptorSet (injecting system protos as well-known
imports), locates the message identified by type_name in the descriptor set, and validates the schema. It then:
1. Looks up the TypeDefinition via the `type_name_unique` index. If not found, creates a new TypeDefinition artifact
   (`type_name`, `current_version_id` unset, `immutable = false` for user types).
2. If a prior TypeVersionDefinition exists for this type (checked via `type_versions_by_type` index), validates schema
   compatibility against the most recent version.
3. Extracts index declarations from the message's custom options. For each key_type, checks the `index_key_type_unique`
   index: if an IndexDefinition already exists, validates compatibility; if not, creates a new IndexDefinition artifact.
4. Extracts reference declarations from field options. For each reference, validates field type, `on_delete`,
   `target_type_name`, and presence of a covering index (reference field as sole key; no where clause in the MVP; post-MVP
   may allow `IS_SET`). For each reference, checks `reference_key_type_unique`: if a ReferenceDefinition already exists,
   validates compatibility; if not, creates a new ReferenceDefinition artifact.
5. Creates a TypeVersionDefinition artifact (type_id = TypeDefinition artifact_id, descriptor_set, proto_source).
6. Derives all index entries for the new artifacts and commits atomically.
7. Returns the new TypeVersionDefinition artifact_id (the version_id).

Schema compatibility rules for launch:
1. Existing fields must not be removed or have their type changed. These rules apply recursively to nested message
   types: removing or changing a field in a nested message is a breaking change. Adding fields to a `oneof` is
   permitted; removing `oneof` fields or the `oneof` itself is a breaking change. Changing a field between `optional`,
   `required`, and `repeated` is a type change.
1. Existing index definitions must not be removed or modified. A modification is any change that would alter what is
   indexed or the shape of the index payload: changes to key fields, order fields, or the unique flag. Such changes
   require a full index rebuild (backfill), which is post-launch. Predicate changes (post-MVP) would also require a
   rebuild.
1. Existing reference declarations must not be removed or modified. A modification includes changes to target_type_name,
   the reference field, the covering index key_type, or the on_delete behavior. Such changes are post-launch.
1. New fields, new indexes, and new reference declarations may be added. New indexes apply only to writes after
   registration (no backfill in the MVP), so index completeness for historical data is not guaranteed.
1. Field number reassignment is rejected.

**ListTypeVersions** fetches the `type_versions_by_type` index for the TypeDefinition's artifact_id, returning version
artifact_ids in creation order.

**SetCurrentTypeVersion** updates the TypeDefinition artifact's `current_version_id` field via UpdateArtifact with
test-and-set semantics. The `expected_version_id` is the current_version_id that the caller expects (use 0 or absent for
the initial set). The registry validates that the target version_id references a TypeVersionDefinition whose type_id
matches the TypeDefinition's artifact_id before applying the update.

RegisterTypeVersion and SetCurrentTypeVersion are separate operations. A registered version that is not yet current is a
valid state — it can be used explicitly by callers that specify a version_id, but will not be resolved by default. Type
registration and current-pointer updates are low-concurrency administrative operations; contention is not expected.

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

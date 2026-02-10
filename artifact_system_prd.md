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
1. Type registry with runtime registration of proto3 schemas, descriptor sets as canonical runtime artifacts, and
   retained custom options including LLM instruction/description annotations at message and field levels.
1. Type version resolution via an optional current pointer (see Type identity and versioning).
1. Artifact server API hides branches and transaction mechanics; callers only see conflicts and resolved storage version
   IDs.

### Non-goals for launch

#### Post-launch goals
1. Partial-update APIs that avoid full rewrites; any modification writes a full new object and duplicates unchanged data.
1. Index sharding and index migrations (backfills/reindexes).
1. Caching layers, triggers, and virtual/computed fields.
1. Application-defined payload merges (including CRDT-based merges) beyond index-derived merges.
1. Extended artifact validation beyond proto3 structural checks (semantic/business rules).
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
1. conflict_type: one of {INDEX_CONFLICT, PAYLOAD_CONFLICT, VERSION_MISMATCH}.
1. retryable: boolean indicating whether the server would retry (false when exhausted or payload conflict).
1. attempts: number of attempts performed.
1. artifacts/indexes involved: artifact_id for payload conflicts; index key (key_type + encoded key) for index conflicts;
   artifact_id for version mismatches.
1. version_ids: base, ours, theirs storage version IDs when available; for VERSION_MISMATCH include expected_version_id
   and current_version_id.

**Exhaustion behavior**
1. If the retry limit is reached while resolving index conflicts, return the conflict response with retryable=false.
1. Callers may re-read state, resolve conflicts at the application level, and retry the operation with updated expected version IDs.

## Artifact Layer

This layer builds on the Storage Layer and provides shared artifact functionality: artifact schemas and types, ID
allocation, indexes, index conflict resolution, and type metadata. Application-specific logic and permissions live in the
App Layer.

### Necessary features for launch

1. Integration with LakeFS
1. Indexes for looking up artifacts (and ability to resolve conflicts)
1. Artifact Type registry (with version and schema support)

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
allocation path during type registration. That artifact_id is used as the key_prefix in index storage paths and is
returned in type metadata. This means IndexDefinition artifacts are created as part of the RegisterTypeVersion
transaction before any user artifacts of that type exist.

An index definition includes:
1. key_type: the name of the index and the identifier passed to fetch index calls. key_type must be globally unique
   across all registered index definitions; this uniqueness constraint is enforced by the registry (not encoded in
   proto3).
2. key: the fields that partition the index. Different key values on the same index type map to different index
   objects.
3. order fields: fields that determine the sort order. Each order field must declare a direction (ASC or DESC) based on
   the field type's sort order. The special field name artifact_id refers to the artifact ID (not a payload field) and
   must be included as an order field to make each row uniquely identifiable. In the MVP, artifact_id is required but
   does not need to be the final order field. This is a simplification of a general unique constraint on index rows.
4. where clauses: predicates that determine whether a value should be indexed. Supported ops include ==, !=, >, <, >=,
   <=, and IS_SET. The LHS must be a field; the RHS can be a field or a constant. The IS_SET operator checks whether an
   `optional` field has explicit presence and requires no RHS; it is the preferred way to gate an index on field
   presence. The != (NE) operator can also serve as a presence gate for `optional` fields (since missing fields evaluate
   to false for any comparison), but IS_SET is unambiguous and does not depend on choosing a sentinel RHS value.
5. unique: whether the index enforces at most one artifact ID per index key.

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
pagination, cursoring, filtering, or query-time predicate evaluation in the MVP.

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

**Where clauses vs fetch-time behavior**
1. where clauses are evaluated only at index build/update time against artifact payloads.
1. Fetch execution never re-evaluates where clauses or artifact payloads; it operates solely on the stored index state.

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

1. `artifacts/{artifact_id}` — artifact payloads. The artifact_id is a uint64 encoded as base64 using the URL-safe
   alphabet of its big-endian bytes without padding.
2. `indexes/{key_prefix}/{key_hash}` — index objects. The key_hash is a SHA-256 content-addressed hash of the encoded
   key bytes; path encoding is defined in Index physical storage.
3. `types/{type_id}/{version_id}` — type definitions. The `type_id` is the artifact ID representing the logical type and
   `version_id` is the artifact ID representing the physical version, both encoded as base64url of big-endian uint64
   bytes without padding (matching the artifact_id encoding). The `current` pointer, if used, is at
   `types/{type_id}/current`.

### Artifacts

Artifacts are stored as opaque payloads defined by Types. Writes use test-and-set via storage version IDs; conflict
ownership (payload vs. index) is defined in the Conflict retry policy. In the future, we can add application-defined
merge logic per artifact type.

#### Artifact contract (minimum)

Artifacts are defined by (artifact_id, type_name, type_version, payload).
1. artifact_id: opaque uint64 allocated by a separate ID allocation service.
2. type_name/type_version: must resolve to a registered type version in the registry.
3. payload: serialized protobuf message (binary wire format) for the resolved type/version.

The artifact_id is metadata and does not need to be duplicated in the payload. If a type schema includes an id field, the
artifact layer should validate that it matches the artifact_id.

On create/update, the artifact layer validates the payload using standard proto3 structural validation for the declared
type/version and validates any type metadata constraints (for example, indexes defined in metadata). Artifact validation
is limited to proto3 structural checks; semantic or business validation is out of scope for launch. Responses return the
resolved type/version and the storage version ID so callers can perform safe updates. Updates and deletes require an
expected storage version ID to avoid lost updates. Reads return payload bytes plus the type name/version and the storage
version ID. Partial updates are out of scope for launch.

The artifact layer stores payload bytes as provided and does not reserialize them. This preserves unknown fields and
avoids any reliance on canonical protobuf binary encodings. Text/JSON formats are not used for storage.

#### Delete semantics and retention

Deletes are logical tombstones. A delete writes a new version at the same artifact key with an empty payload, advances
the storage version ID, and returns that version to the caller.

1. Empty payloads are reserved for tombstones. The artifact layer rejects zero-length payloads for live artifacts; if a
   type needs to represent an "empty" value, include a sentinel field or wrap it in an envelope message.
1. Index handling: deleting an artifact removes all derived index entries for that artifact ID (as if the artifact no
   longer matches any where clauses). Unique indexes free the slot. If an index becomes empty, it is tombstoned by
   storing an index object with an empty value portion (row_count = 0 and no value entries) so merges can detect
   deletions.
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

#### Type taxonomy and bootstrapping

In this document, "type" applies to both artifact payloads and definition objects:
1. Artifact types: schemas for artifact payloads.
2. Definition types: schemas for definition objects (artifact definitions and index definitions).
3. TypeDefinition: the schema for type definitions themselves.

Definition objects are represented as protobuf messages and are usually embedded as type metadata via custom options. If
we store standalone definition objects as artifacts (for example, for migrations), their payloads conform to their
definition type (ArtifactDefinition, IndexDefinition, etc.). TypeDefinition is a built-in meta-type used to describe
these definition types; it is bootstrapped in the artifact layer and treated as immutable.

Artifact types are stored in LakeFS so they are tied to the repo state. This enables migration transactions
(post-launch): a new branch can add an index, backfill it for existing data, and merge only when the index is consistent
with all changes since the fork point. The same mechanism supports index removal and schema migrations.

Type definitions are stored per version at `types/{type_id}/{version_id}`, where `type_id` and `version_id` are artifact
IDs (see Object namespaces and paths for encoding). Each version stores the descriptor set
alongside the original .proto source for inspection and re-compilation. Metadata such as index definitions, viewer
endpoints, and LLM instructions are attached via `extend google.protobuf.MessageOptions`. The registry automatically
imports option extensions when loading a type definition. See Protocol buffers as the type definition for details.

An artifact definition includes the following metadata:
1. Schema: defined by the protobuf message itself.
2. Indexes: a repeated MessageOption listing index definitions; a FieldOption for single-field indexes may be added later.
3. Actions: a MessageOption defining a dictionary of actions available on artifacts of this type. Stored as metadata at
   registration; interpretation is an App Layer concern and is not consumed by the artifact layer at launch.
4. Viewer: a MessageOption defining the default viewer endpoint. Same as Actions — stored but not interpreted at launch.
5. Custom Instruction: a MessageOption defining LLM instructions for the type.

#### Protocol buffers as the type definition

Protocol buffers are the canonical representation for types. A type version stores a compiled
google.protobuf.FileDescriptorSet that includes the defining .proto and all transitive imports (including option
extensions). The descriptor set is authoritative for parsing, validation, and index derivation. The registry must accept
new .proto definitions at runtime and compile them into a descriptor set on registration. The original .proto source
should be stored alongside the descriptor set for inspection and re-compilation, but the descriptor set is the required
runtime artifact.

Standard protobuf validation for registration is: compile with protoc, reject parse/descriptor errors, and only accept
proto3 syntax for launch. Runtime compilation should enforce resource limits (maximum input file size, compilation
timeout, nesting depth) to prevent resource exhaustion from malformed or adversarial schemas. Custom options are
supported (see below) and must be retained at runtime so that metadata is available via descriptors.

#### Custom options as metadata

Type metadata is defined using protobuf custom options (extensions). In proto3, extensions are permitted only for custom
options, which aligns with our needs.
1. Message options: indexes, actions, viewer endpoint, LLM instructions.
2. Field options (optional): syntactic sugar for single-field indexes.
3. Virtual fields (planned): a MessageOption for declaring computed fields; the expression AST schema is to be defined.

Custom options must use runtime retention (not source-only retention) so they appear in the descriptor set. The registry
loads the descriptor set with the option extensions so the artifact layer can read metadata deterministically.

#### Index options schema (example)

The type-level option indexes is a repeated list of IndexDefinition objects. Each `option (indexes) = { ... }` entry
defines one index for the enclosing message type.

OrderDefinition.direction is required; the registry rejects index definitions with ORDER_BY_UNSPECIFIED.

WhereClause.op is required; the registry rejects where clauses with OP_UNSPECIFIED. IS_SET requires no RHS (rhs must be
unset); the registry rejects IS_SET clauses that specify an rhs_field or rhs_value. IS_SET is only valid on fields with
explicit presence (`optional`); the registry rejects IS_SET on implicit-presence scalars.

```proto
syntax = "proto3";

import "google/protobuf/descriptor.proto";

message Constant {
  oneof value {
    string string_value = 1;
    int64 int64_value = 2;
    uint64 uint64_value = 3;
    bool bool_value = 4;
    double double_value = 5;
  }
}

message OrderDefinition {
  string field = 1;
  enum OrderBy {
    ORDER_BY_UNSPECIFIED = 0;
    ASCENDING = 1;
    DESCENDING = 2;
  }
  OrderBy direction = 2;
}

message WhereClause {
  string lhs = 1;
  enum Op {
    OP_UNSPECIFIED = 0;
    GT = 1;
    LT = 2;
    LTE = 3;
    GTE = 4;
    EQ = 5;
    NE = 6;
    IS_SET = 7;
  }
  Op op = 2;
  oneof rhs {
    string rhs_field = 3;
    Constant rhs_value = 4;
  }
}

message IndexDefinition {
  string key_type = 1;
  repeated string key = 2;
  repeated OrderDefinition order = 3;
  repeated WhereClause where = 4;
  bool unique = 5;
}

extend google.protobuf.MessageOptions {
  repeated IndexDefinition indexes = 50002;
}

message DataFrameArtifact {
  option (indexes) = { key_type: "by_owner" key: ["created_by"] order: { field: "artifact_id" direction: ASCENDING } };
  option (indexes) = { key_type: "by_repo_created_by" key: ["repo_id", "created_by"] order: { field: "artifact_id" direction: ASCENDING } };

  uint64 created_by = 1;
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

Index and predicate evaluation use protobuf field semantics:
1. Optional fields have explicit presence; if unset, the field is treated as missing for where clauses and index keys.
   A missing field causes any where clause comparison involving it to evaluate to false (the artifact is excluded from
   the index). This is not three-valued logic; there is no "unknown" — missing is simply non-matching.
2. Implicit-presence scalar fields (proto3 without optional) have no presence; missing is indistinguishable from the
   default value. For any field used in predicates or index keys, prefer optional.
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

Types are identified by (type_name, type_version). Type versions are immutable once registered; any schema or metadata
change is a new version. The canonical storage path for a version is `types/{type_id}/{version_id}` (see Object
namespaces and paths). Optionally, a small pointer object may be stored at `types/{type_id}/current` to identify the
default version. If a CRUD request omits type_version, the artifact layer resolves to the current pointer at request time
and returns the resolved version in the response for reproducibility.

### Launch API surface (minimum)

The Artifact Layer exposes a minimal API surface for launch and uses gRPC for service-to-service communication. The
server hides branches and transaction mechanics; callers only see resolved storage version IDs and structured
conflict responses. Client-to-app-layer transport remains application-specific and will be decided per application.

#### Artifact CRUD API

Operations (gRPC service-to-service; client-to-app transport is application-specific):
1. CreateArtifact(type_name, type_version?, payload)
2. GetArtifact(artifact_id)
3. BatchGetArtifacts(artifact_ids)
4. UpdateArtifact(artifact_id, expected_version_id, type_name, type_version?, payload)
5. DeleteArtifact(artifact_id, expected_version_id)

Request/response expectations:
1. artifact_id is allocated by the artifact layer via a separate ID service and returned in Create responses.
1. type_version is optional for Create/Update; resolution behavior is defined in Type identity and versioning.
1. Update/Delete require expected storage version ID (test-and-set per Artifact contract); mismatches return a
   VERSION_MISMATCH conflict.
1. Reads return payload bytes plus type_name/type_version and storage version ID.
1. BatchGetArtifacts returns results in the same order as the input IDs using explicit per-id results (for example, a
   `oneof { artifact, not_found }` wrapper) so missing or tombstoned artifacts preserve positional correlation.
1. Create/Update/Delete return the resolved type_version (when applicable) and the new storage version ID.
1. Delete and tombstone behavior is defined in Delete semantics and retention.

Conflict/error behavior: conflict types, response payloads, and auto-resolution eligibility are defined in the Conflict
retry policy (MVP). The conflict response uses conflict_type values INDEX_CONFLICT, PAYLOAD_CONFLICT, and
VERSION_MISMATCH.

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

The registry supports registering and resolving type versions:
1. RegisterTypeVersion(type_name, version, schema, metadata)
1. GetTypeVersion(type_name, version)
1. ListTypeVersions(type_name)
1. SetCurrentTypeVersion(type_name, version, expected_current_version) (optional)
1. ResolveTypeVersion(type_name) -> version (current)

RegisterTypeVersion performs protobuf schema validation and validates metadata (for example, index definitions reference
existing fields and supported types). For each new index definition, the registry creates an IndexDefinition artifact
(via standard ID allocation) and stores it as part of the registration transaction; the assigned artifact_id is returned
in type metadata and used as key_prefix in index storage paths. When a prior version exists for the same type_name, the
registry validates schema compatibility against the most recent version. Compatibility rules for launch:
1. Existing fields must not be removed or have their type changed. These rules apply recursively to nested message
   types: removing or changing a field in a nested message is a breaking change. Adding fields to a `oneof` is
   permitted; removing `oneof` fields or the `oneof` itself is a breaking change. Changing a field between `optional`,
   `required`, and `repeated` is a type change.
1. Existing index definitions must not be removed or modified. A modification is any change that would alter what is
   indexed or the shape of the index payload: changes to key fields, order fields, where clauses, or the unique flag.
   Such changes require a full index rebuild (backfill), which is post-launch.
1. New fields and new indexes may be added. New indexes apply only to writes after registration (no backfill in the MVP),
   so index completeness for historical data is not guaranteed. Where clauses should be used to make the index valid for
   the subset of artifacts it claims to cover.
1. Field number reassignment is rejected.

The registry stores the protobuf schema (source or descriptor) plus any required imports and extensions so it can be
loaded deterministically by the artifact layer. SetCurrentTypeVersion requires `expected_current_version` — the type
version string that `current` currently points to — to prevent lost updates via compare-and-swap. This is distinct from
the LakeFS storage version ID used for artifact test-and-set; it compares the logical type version pointer.

RegisterTypeVersion and SetCurrentTypeVersion are separate operations. A registered version that is not yet current is a
valid state — it can be used explicitly by callers that specify a type_version, but will not be resolved by default. Type
registration and current-pointer updates are low-concurrency administrative operations; contention is not expected.

## App Layer

The App Layer applies domain-specific rules, permissions, and workflows on top of the Artifact Layer. Its design is out
of scope for this document and will be covered in a separate PRD. An example App Layer service is an Artifact Viewer.

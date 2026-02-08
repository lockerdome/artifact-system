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
repo (see Dependencies and constraints). The Spark vacuum process removes commits that are not recent and not pinned by a
tag or referenced as a branch head. This is the default recommended behavior; to preserve older commits, pin them with
tags or adjust the retention window.

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
1. conflict_type: one of {INDEX_CONFLICT, PAYLOAD_CONFLICT}.
1. retryable: boolean indicating whether the server would retry (false when exhausted or payload conflict).
1. attempts: number of attempts performed.
1. artifacts/indexes involved: artifact_id for payload conflicts; index key (key_type + encoded key) for index conflicts.
1. version_ids: base, ours, theirs storage version IDs when available.

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

Concepts such as tags and groups are higher-level abstractions built on indexes. The specific fields indexed depend on the
artifact type, but the underlying index storage format is uniform. Index objects are subject to merge conflicts because
any concurrent artifact write can update the same index object. Conflict resolution uses a three-way diff between the
merge base and both heads (see Conflict model). Empty indexes are tombstoned so that merges can detect the deletion (see
Delete semantics and retention).

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
1. key_type: the name of the value and the identifier passed to fetch index calls.
2. key: the fields that partition the index. Different key values on the same index type map to different index
   objects.
3. order fields: fields that determine the sort order. Each order field must declare a direction (ASC or DESC) based on
   the field type's sort order. The special field name artifact_id refers to the artifact ID (not a payload field) and
   must be included as the final order field to guarantee uniqueness. This is a simplification of a general unique
   constraint on index rows.
4. where clauses: predicates that determine whether a value should be indexed. Supported ops include ==, !=, >, <, >=,
   <=. The LHS must be a field; the RHS can be a field or a constant. The != (NE) operator is useful for "field is
   present" filtering: since missing fields evaluate to false for any comparison, a where clause like
   `{ lhs: "foo" op: NE rhs_value: { ... } }` effectively gates the index on field presence.
5. unique: whether the index enforces at most one artifact ID per index key.

#### Index merge semantics

When a merge reports a conflicting index object, we perform a three-way merge using the merge base and both heads. Each
index object is treated as a set of entries keyed by artifact ID with associated order field values. For each branch we
compute adds (head minus base) and removes (base minus head). If the same artifact ID exists in both but with different
order values, treat that as a remove and an add. We apply all removes to the base, then apply all adds, de-duplicate by
artifact ID, and re-sort by the configured order fields (tie-breaker: artifact ID). This yields a deterministic result
and is idempotent across retries. If a unique index (see Index definition) ends up with more than one item, the merge
fails and the conflict is returned to the calling service for resolution.

#### Index physical storage

Index objects are stored under indexes/{key_prefix}/{encoded_keys}. The key_prefix is the uint64 IndexDefinition artifact ID encoded as base64 using the URL-safe alphabet of its big-endian uint64 bytes without padding.
The encoded_keys is base64 using the URL-safe alphabet of the concatenated binary encodings for each key field.
Variable-length fields (for example, strings) must include length prefixes in their binary encoding.

#### Index key/value encoding (MVP)

Index keys and order-field columns use a deterministic binary encoding independent of protobuf wire encoding. Encoding
is little-endian to enable zero-copy access on common hosts. Ordering comparisons use the field's native type semantics
(numeric order for numbers, lexicographic for strings/bytes) on decoded values, not raw bytes. This means byte-level
comparison (for example, memcmp on encoded keys) is not valid for ordering; index merge logic and any sorted operations
must decode values before comparing. If byte-level key ordering is needed in the future (for example, for range
partitioning or sharded bucket assignment), the encoding would need to change to an order-preserving format (such as
big-endian for unsigned integers). This is acceptable for the MVP since all comparisons are decode-then-compare.

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

1. If a key or order field references a scalar sub-field of a message, encode the referenced scalar using the same rules.

For encoded_keys, concatenate key field encodings in order. For columnar order-field values, each row value is encoded
using the same rules; variable-length values include length prefixes to preserve row boundaries. Sorting and comparison
operate on decoded values.

#### Index object representation

Index objects must be encoded deterministically to make diffs and merges reliable. The value is a column-oriented binary
format with one column per order field. Rows are ordered by the order fields, which must
include artifact_id as the final field. The encoding format can be configured per index. By default, each column is
stored as an array of the raw binary values. In the future, we can support dictionary encoding or RLE per column to
compress the arrays. Empty indexes are tombstoned so that merges can detect the deletion (see Delete semantics and
retention).

#### Index fetch behavior (MVP)

The index fetch API (FetchIndex) provides a raw fetch of the materialized index results for a specific index key. There is no
pagination, cursoring, filtering, or query-time predicate evaluation in the MVP.

**Inputs**
1. key_type: identifies which index definition to query.
1. key values: the complete set of key fields defined by the index, provided as typed values. All key fields are
   required; partial keys are rejected.

**Execution semantics**
1. The artifact layer locates the index object for (key_type, encoded key values) and reads its current state.
1. Results are returned in the deterministic order defined by the index’s order fields. Ordering stability is defined
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
1. The value message contains the ordered columns defined by the index (ending with artifact_id).
1. Artifact payloads are embedded or referenced according to the index value schema defined for that index.

The response may also include the index object’s storage version ID for debugging or consistency checks.

**Uniqueness**
1. Unique index responses contain zero or one entry (see Index definition for the uniqueness invariant).

#### Index sharding (planned)

When indexes become large, shard them by bucket and store a small manifest object that lists all bucket objects for an
index key. Bucket naming should be deterministic (for example, a fixed-width prefix of a hash) so that reads can page
over buckets predictably. The manifest is the canonical entrypoint for reads.

### Object namespaces and paths

Object paths are a private implementation detail and must not be exposed to end-users. The top-level namespaces are:

1. `artifacts/{artifact_id}` — artifact payloads. The artifact_id is a uint64 encoded as base64 using the URL-safe
   alphabet of its big-endian bytes without padding.
2. `indexes/{key_prefix}/{encoded_keys}` — index objects. Path encoding is defined in Index physical storage.
3. `types/{type_name}/{version}` — type definitions. The `current` pointer, if used, is at `types/{type_name}/current`.

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
   longer matches any where clauses). Unique indexes free the slot. If an index becomes empty, it is tombstoned (written
   as an empty payload) so that merges can detect the deletion.
1. Reads treat tombstones as not found for standard Get. There is no public restore or audit API in the MVP; to
   "undelete" a caller writes a new payload with the tombstone's expected version ID, creating a new version in history.
1. Retention: history is preserved as LakeFS commits but is subject to LakeFS GC/vacuum. To guarantee audit/restore
windows, pin commits/tags or configure retention accordingly. Hard delete/purge is out of scope for launch.

#### Consistency and transaction semantics

The Artifact Layer presents a single canonical branch to callers; branch and transaction mechanics are internal.

1. Each Create/Update/Delete runs as an internal transaction and is merged before the call returns success.
1. Read-after-write: once a write call succeeds, subsequent reads of that artifact key return the committed version and its storage version ID.
1. Consistency is per artifact key plus its derived index updates; there are no multi-key or cross-artifact atomic transactions in the MVP.
1. Concurrent writes may diverge internally; conflict ownership is defined in the Conflict retry policy.
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

Type definitions are stored as protobuf files at `types/{type_name}`. Metadata such as index definitions, viewer
endpoints, and LLM instructions are attached via `extend google.protobuf.MessageOptions`. The registry automatically
imports option extensions when loading a type definition. See Protocol buffers as the type definition for details.

An artifact definition includes the following metadata:
1. Schema: defined by the protobuf message itself.
2. Indexes: a repeated MessageOption listing index definitions; a FieldOption for single-field indexes may be added later.
3. Actions: a MessageOption defining a dictionary of actions available on artifacts of this type.
4. Viewer: a MessageOption defining the default viewer endpoint.
5. Custom Instruction: a MessageOption defining LLM instructions for the type.

#### Protocol buffers as the type definition

Protocol buffers are the canonical representation for types. A type version stores a compiled
google.protobuf.FileDescriptorSet that includes the defining .proto and all transitive imports (including option
extensions). The descriptor set is authoritative for parsing, validation, and index derivation. The registry must accept
new .proto definitions at runtime and compile them into a descriptor set on registration. The original .proto source
should be stored alongside the descriptor set for inspection and re-compilation, but the descriptor set is the required
runtime artifact.

Standard protobuf validation for registration is: compile with protoc, reject parse/descriptor errors, and only accept
proto3 syntax for launch. Custom options are supported (see below) and must be retained at runtime so that metadata is
available via descriptors.

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

WhereClause.op is required; the registry rejects where clauses with OP_UNSPECIFIED.

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

Index payloads are stored as proto3 messages that wrap the columnar binary values. Columns are stored as raw bytes to
enable typed-array handling in JavaScript and efficient scans in backend languages. Index keys are encoded in the object
path and are not stored in the payload; when a composite object is needed, the service can reconstruct the key from the
path and assemble a key/value envelope without rewriting the stored payload.

For the DataFrameArtifact example, the by_owner index uses key = created_by and the by_repo_created_by index uses
key = (repo_id, created_by). The generated schemas for those indexes are:

```proto
syntax = "proto3";

message IndexValue_DataFrameArtifact_by_owner {
  uint32 row_count = 1;
  bytes artifact_id = 2;
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
  bytes artifact_id = 2;
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

If an index has additional order fields, include one bytes column per order field in order (ending with artifact_id).

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
   fields require a scalar sub-field to be referenced.
5. Map fields are not indexable for launch; model indexable map-like data as repeated entry messages.

#### Type identity and versioning

Types are identified by (type_name, type_version). Type versions are immutable once registered; any schema or metadata
change is a new version. The canonical storage path for a version is types/{type_name}/{version}. Optionally, a small
pointer object may be stored at types/{type_name}/current to identify the default version. If a CRUD request omits
type_version, the artifact layer resolves to the current pointer at request time and returns the resolved version in the
response for reproducibility.

### Launch API surface (minimum)

The Artifact Layer exposes a minimal API surface for launch and uses gRPC for service-to-service communication. The
server hides branches and transaction mechanics; callers only see resolved storage version IDs and structured
conflict responses. Client-to-app-layer transport remains application-specific and will be decided per application.

#### Artifact CRUD API

Operations (conceptual names; transport is TBD):
1. CreateArtifact(type_name, type_version?, payload)
2. GetArtifact(artifact_id)
3. BatchGetArtifacts(artifact_ids)
4. UpdateArtifact(artifact_id, expected_version_id, type_name, type_version?, payload)
5. DeleteArtifact(artifact_id, expected_version_id)

Request/response expectations:
1. artifact_id is allocated by the artifact layer via a separate ID service and returned in Create responses.
1. type_version is optional for Create/Update; resolution behavior is defined in Type identity and versioning.
1. Update/Delete require expected storage version ID (test-and-set per Artifact contract); mismatches return a conflict.
1. Reads return payload bytes plus type_name/type_version and storage version ID.
1. BatchGetArtifacts returns results in the same order as the input IDs. Missing or tombstoned artifacts are represented
   as absent entries (not errors) so callers can correlate by position.
1. Create/Update/Delete return the resolved type_version (when applicable) and the new storage version ID.
1. Delete and tombstone behavior is defined in Delete semantics and retention.

Conflict/error behavior: conflict types, response payloads, and auto-resolution eligibility are defined in the Conflict
retry policy (MVP). The conflict response uses conflict_type values INDEX_CONFLICT and PAYLOAD_CONFLICT.

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
1. SetCurrentTypeVersion(type_name, version, expected_version_id) (optional)
1. ResolveTypeVersion(type_name) -> version (current)

RegisterTypeVersion performs protobuf schema validation and validates metadata (for example, index definitions reference
existing fields and supported types). For each new index definition, the registry creates an IndexDefinition artifact
(via standard ID allocation) and stores it as part of the registration transaction; the assigned artifact_id is returned
in type metadata and used as key_prefix in index storage paths. When a prior version exists for the same type_name, the
registry validates schema compatibility against the most recent version. Compatibility rules for launch:
1. Existing fields must not be removed or have their type changed.
1. Existing index definitions must not be removed or modified. A modification is any change that would alter what is
   indexed or the shape of the index payload: changes to key fields, order fields, where clauses, or the unique flag.
   Such changes require a full index rebuild (backfill), which is post-launch.
1. New fields and new indexes may be added.
1. Field number reassignment is rejected.

The registry stores the protobuf schema (source or descriptor) plus any required imports and extensions so it can be
loaded deterministically by the artifact layer. SetCurrentTypeVersion requires an expected prior version (etag) to
prevent lost updates.

## App Layer

The App Layer applies domain-specific rules, permissions, and workflows on top of the Artifact Layer. Its design is out
of scope for this document and will be covered in a separate PRD. An example App Layer service is an Artifact Viewer.

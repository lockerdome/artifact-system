# Layered Artifact System

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

LakeFS can be set up with the provision.sh script. This will create a MIG, Postgres database, Load Balancer, Image,
Instance Template, Health Check, Spark vacuum Service, VPC, and a few other odds and ends which enable a highly
available LakeFS cluster. Note that the script uses two zones with the MIG to ensure high availability and utilizes
the HA version of Postgres. The costs are low enough that this seems like a worthwhile choice. The script will also
create a bucket in Cloud Storage called lakefs-data-{project-id}. You will probably want to set the PROJECT_ID before
running the provisioning script and check that the IP ranges will work with our broader system.

The secret needed to login with the admin account is saved in the secret manager as lakefs-secret-access-key and
the access key id is static and available (or changeable) by updating the ACCESS_KEY_ID value in the provision script.
With these, you will be able to login to the LakeFS admin portal, but, due to how locked down everything is, you will
need to tunnel using IAP and SSH then use that connection to locally bind a port which gets forwarded along to one of
the instances for communicating with the http server. Assuming that the provision script runs successfully, it will
provide instructions at the end.

I would suggest that we create a repo in LakeFS for the artifacts. It is important to note that if we need to have
transactions across more than artifacts, all those will need to be in the same repo of LakeFS. It does not support
cross-repo transactions. Additionally, it is important to note that the Spark cleanup process will remove commits which
are not recent and not directly pinned by a tag or referenced as the head of a branch. We could choose to change this
behavior, but, the default they recommend will "vacuum" anything older than the time window and which is not referenced.

### Conflict model

LakeFS is designed to be general purpose and, as such, it does not assume that it knows how to merge when objects
diverge in two branches. This is unlikely to cause a surprise when updating an artifact, however, because we will be
using objects to store the state of indexes, those will be very likely to have conflicts. There are two tools available
here: first, we can use pre-merge hooks; I don't recommend those for conflict resolution and, given that we are adding
our own layer on top of LakeFS, I suspect that this feature will be unhelpful for us. The second tool, and the one I
recommend, is to handle the conflict after trying to merge. To do this, simply create logic that when a transaction
attempts to merge, if the LakeFS server returns with a Conflict, read through the conflicts, resolve index conflicts
with artifact layer logic and then retry the merge. Repeat this until it merges successfully or too many tries are
reached. The latter should not really ever occur in production with our current levels of concurrency.

## Artifact Layer

This layer builds on the Storage Layer and provides shared artifact functionality above storage. Specifically, the
artifact layer must support artifact schemas (and types), ID allocation, indexes, groups, resolving merge conflicts for
indexes, providing metadata for an artifact type, caching in redis, and other shared functionality. Application-
specific logic and permissions live in the App Layer.

### Necessary features for launch

1. Integration with LakeFS
1. Indexes for looking up artifacts (and ability to resolve conflicts)
1. Artifact Type registry (with version and schema support)

### Additional features which should be planned for

1. Caching objects in redis
1. Sharding of indexes for when they get extremely large (think "get me all artifacts of type X")
1. Migrating indexes (adding or removing indexes)
1. Schema validation for artifacts
1. Supporting pulling of "part" of an artifact

### Indexes for looking up artifacts

We have discussed having tags or groups which are both essentially higher level abstractions around the idea of an
index. While what we index on will likely be at least partially driven by the particular artifact type, the underlying
way we store the artifacts (or singular artifact when an index is unique) which are present in each index will likely
be uniform. I would suggest a similar approach to what we use in ReactDB. Of note, however, any changes to an index will
conflict, so merging will require doing a diff between the common base, current head we are merging into and the current
head of what we are merging for each conflicting index. (See Conflict model above) In other words, we will need to read the
changes and reconstruct what the edits are for each diverging branch and then apply all those edits. If the index is a
unique index, we would obviously require that any final version contains zero or one item. Empty indexes should simply
not exist at HEAD (though history may include prior objects).

Indexes are derived from artifacts by artifact layer logic and are never manually set. They only conflict on merge.
The commit workflow reads the latest index state, applies adds/removes derived from the artifact changes, and writes
back the updated index object(s). Unique indexes enforce at most a single item across all commits and merges.

#### Index definition

Index definitions are expressed as protobuf messages (IndexDefinition) and attached to artifact types via a
message-level custom option (indexes). The IndexDefinition schema describes the fields below.

An index definition includes:
1. key_type: the name of the value and the identifier passed to fetch index calls.
2. key_prefix: a numeric prefix inside the indexes keyspace. This defaults to a stable hash of key_type.
3. key: the fields that partition the index. Different key values on the same index type map to different index
   objects.
4. order fields: fields that determine the sort order. Each order field must declare a direction (ASC or DESC) based on
   the field type's sort order. The special field name artifact_id refers to the artifact ID (not a payload field) and
   must be included as the final order field to guarantee uniqueness. This is a simplification of a general unique
   constraint on index rows.
5. where clauses: predicates that determine whether a value should be indexed. Supported ops include ==, >, <, >=, <=.
   The LHS must be a field; the RHS can be a field or a constant.
6. unique: whether the index enforces at most one artifact ID per index key.

#### Index merge semantics

When a merge reports a conflicting index object, we perform a three-way merge using the merge base and both heads. Each
index object is treated as a set of entries keyed by artifact ID with associated order field values. For each branch we
compute adds (head minus base) and removes (base minus head). If the same artifact ID exists in both but with different
order values, treat that as a remove and an add. We apply all removes to the base, then apply all adds, de-duplicate by
artifact ID, and re-sort by the configured order fields (tie-breaker: artifact ID). This yields a deterministic result
and is idempotent across retries. If a unique index ends up with more than one item, the merge fails and the conflict is
returned to the calling service for resolution.

#### Index physical storage

Index objects are stored under indexes/{key_prefix}/{encoded_keys}. The key_prefix is an unsigned numeric value (default:
stable hash of key_type) encoded as base64 using the URL-safe alphabet of its big-endian uint64 bytes without padding.
The encoded_keys is base64 using the URL-safe alphabet of the concatenated binary encodings for each key field.
Variable-length fields (for example, strings) must include length prefixes in their binary encoding.

#### Index object representation

Index objects must be encoded deterministically to make diffs and merges reliable. The value is a column-oriented binary
format with one column per order field. Rows are ordered by the order fields, which must
include artifact_id as the final field. The encoding format can be configured per index. By default, each column is
stored as an array of the raw binary values. In the future, we can support dictionary encoding or RLE per column to
compress the arrays. Empty indexes simply do not exist at HEAD (though history may include prior objects).

#### Index sharding (planned)

When indexes become large, shard them by bucket and store a small manifest object that lists all bucket objects for an
index key. Bucket naming should be deterministic (for example, a fixed-width prefix of a hash) so that reads can page
over buckets predictably. The manifest is the canonical entrypoint for reads.

### Object namespaces and paths

In order to support indexes, we need to ensure that they will never overlap with artifacts, but they must be in the same
path structure where we store all objects. I suggest something simple like this: use artifacts/{artifact id} for the
artifacts themselves and indexes/{key_prefix}/{encoded_keys} for the indexes. We could also have types/{artifact type name}
be where we store the artifact type information. We could also store types/{artifact type name}/{version} when we wish
to version these. I would strongly recommend against making any of these details transparent to end-users and would
treat the actual paths of objects in cloud storage as a private implementation detail.

Artifact paths are based on ID and do not include special characters. IDs are likely to be uint_64 and will be encoded
when used in a path (for example, base64 encoding with the URL-safe alphabet of the big-endian binary representation
without padding) to avoid special characters in the storage layer.

Index paths are determined based on key_prefix and key values. The key_prefix is encoded as base64 using the URL-safe
alphabet of its big-endian uint64 bytes without padding and used as the first path segment under indexes/. The
encoded_keys is base64 using the URL-safe alphabet of the concatenated binary encodings of each key field
(length-prefixed where needed).

### Artifacts

Artifacts are stored as opaque payloads defined by Types; the artifact layer does not auto-resolve payload conflicts.
Writes use test-and-set via an object version ID (for example, an ETag). If the precondition fails or a merge conflict
occurs, we return a conflict to the calling service so it can resolve and retry. Only index conflicts are auto-resolved.
In the future, we can add application-defined merge logic per artifact type.

#### Artifact contract (minimum)

Artifacts are defined by (artifact_id, type_name, type_version, payload).
1. artifact_id: opaque uint64 allocated by a separate ID allocation service.
2. type_name/type_version: must resolve to a registered type version in the registry.
3. payload: serialized protobuf message (binary wire format) for the resolved type/version.

The artifact_id is metadata and does not need to be duplicated in the payload. If a type schema includes an id field, the
artifact layer should validate that it matches the artifact_id.

On create/update, the artifact layer validates the payload using standard protobuf validation for the declared
type/version and validates any type metadata constraints (for example, indexes defined in metadata). Responses return
the resolved type/version and the storage version ID so callers can perform safe updates. Updates and deletes require an
expected storage version ID to avoid lost updates. Reads return payload bytes plus the type name/version and the storage
version ID. Partial updates are out of scope for launch.

The artifact layer stores payload bytes as provided and does not reserialize them. This preserves unknown fields and
avoids any reliance on canonical protobuf binary encodings. Text/JSON formats are not used for storage.

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

We likely want to represent the artifact types as tied to the current state of the repo. This is helpful because we can
create migration transactions and detect when merging if something needs to be corrected before merging. For example, if
we add an index, we can create a new branch with the index and have it populate that index for all the existing data. On
merge, it will look at the branch and detect any changes since it was forked which would require adjustments to that
index and only merge where everything lines up. We can use a similar process when removing an index. If we want to force
all artifacts of a particular version of artifact schema to migrate, we can also create this migration transaction. That
said, we do not need this to be available at launch. It is just the reason why we will want to represent the artifact
schemas in LakeFS.

For the actual storing of artifact type information, we could use protobuf for the schema itself. This would then encode
what will actually be stored in the artifact's object. We can attach metadata in individual fields as needed to define
things like "index this field". We can also use `extend google.protobuf.MessageOptions` to add functionality into the
specific type information. I'd suggest we make the contents of the objects stored in types/{artifact type name} a valid
protobuf definition. We'll have to decide how we load this, though, because we may want to automatically import the
extensions to message metadata and other basic types. We can store fields like viewer endpoints and actions as metadata
which is defined in our extensions to message options such that each artifact type is able to specify those.

As far as specific fields needed in an artifact definition, I think we would need the following:
1. Schema (taken care of by protobuf)
2. Indexes (add a MessageOption which allows creation of one or more indexes; a FieldOption can be added later as
   syntactic sugar for single-field indexes)
3. Actions (add a MessageOption for defining a dictionary of actions to an artifact)
4. Viewer (add a MessageOption for defining the default viewer endpoint)
5. Custom Instruction (add a MessageOption for defining instructions for LLMs)

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
  optional uint32 key_prefix = 2;
  repeated string key = 3;
  repeated OrderDefinition order = 4;
  repeated WhereClause where = 5;
  bool unique = 6;
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
2. Implicit-presence scalar fields (proto3 without optional) have no presence; missing is indistinguishable from the
   default value. For any field used in predicates or index keys, prefer optional.
3. Message-typed fields always have presence; they can be indexed only if a specific scalar sub-field is referenced.
4. Repeated scalar fields generate one index entry per value; repeated message fields require a scalar sub-field to be
   referenced.
5. Map fields are not indexable for launch.

#### Type identity and versioning

Types are identified by (type_name, type_version). Type versions are immutable once registered; any schema or metadata
change is a new version. The canonical storage path for a version is types/{type_name}/{version}. Optionally, a small
pointer object may be stored at types/{type_name}/current to identify the default version. If a CRUD request omits
type_version, the artifact layer resolves to the current pointer at request time and returns the resolved version in the
response for reproducibility.

#### Type registry API surface (minimum)

The registry must support registering and resolving type versions:
1. RegisterTypeVersion(type_name, version, schema, metadata)
2. GetTypeVersion(type_name, version)
3. ListTypeVersions(type_name)
4. SetCurrentTypeVersion(type_name, version) (optional)
5. ResolveTypeVersion(type_name) -> version (current)

RegisterTypeVersion performs protobuf schema validation and validates metadata (for example, index definitions reference
existing fields and supported types). The registry stores the protobuf schema (source or descriptor) plus any required
imports and extensions so it can be loaded deterministically by the artifact layer.

## App Layer

The app layer applies domain-specific rules and permissioning on top of the artifact layer.

### Additional features which should be planned for

1. Some form of validation of permissions
1. Triggers

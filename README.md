# Artifact System

The Artifact System is a versioned, typed data store built on Protocol Buffers. It provides transactional CRUD operations, automatic index maintenance, referential integrity enforcement, and point-in-time consistent reads.

## Core Concepts

- **Artifact**: A data object identified by a unique `artifact_id`. Each artifact has a type, a schema version, and a protobuf-serialized payload.
- **Type**: A registered protobuf message schema. Types are registered at runtime by providing `.proto` source text. Each type can have multiple schema versions (additive changes only).
- **Index**: A derived, automatically maintained data structure declared on a type's proto message. Indexes enable lookups by key fields and are updated on every write.
- **Snapshot**: A frozen, point-in-time read view. All reads from the same snapshot are guaranteed consistent.
- **Transaction**: An atomic unit of work. Reads within a transaction see the transaction's own writes. On success the transaction commits; on failure it rolls back.

## Table of Contents

- [Defining Types](#defining-types)
- [Defining Indexes](#defining-indexes)
- [Defining References](#defining-references)
- [JavaScript Client](#javascript-client)
  - [Setup](#setup)
  - [Registering Types](#registering-types)
  - [Creating Artifacts](#creating-artifacts)
  - [Reading Artifacts](#reading-artifacts)
  - [Querying via Indexes](#querying-via-indexes)
  - [Updating Artifacts](#updating-artifacts)
  - [Deleting Artifacts](#deleting-artifacts)
  - [Nested Transactions](#nested-transactions)
  - [Transaction Snapshots](#transaction-snapshots)
  - [Type and Index Introspection](#type-and-index-introspection)
  - [Error Handling](#error-handling)
- [Constraints and Gotchas](#constraints-and-gotchas)

---

## Defining Types

Types are defined as proto3 messages. The `.proto` source is passed as a string at registration time -- no local `protoc` compilation is needed.

```proto
syntax = "proto3";
package todo;

import "artifact_options.proto";

message TodoList {
  string name = 1;
  string description = 2;
}
```

### Field Types

All standard proto3 scalar types are supported: `int32`, `uint32`, `int64`, `uint64`, `float`, `double`, `bool`, `string`, `bytes`, `enum`, and their signed/fixed variants. Nested messages, `optional` fields, `repeated` fields, and `oneof` groups are also supported.

### Schema Evolution

When registering a new version of an existing type:

- **Allowed**: Adding new fields, adding new indexes, adding new references.
- **Not allowed**: Removing fields, changing field types, changing field numbers, changing field labels (`optional`/`repeated`), removing indexes or references.

## Defining Indexes

Indexes are declared as message-level options using `option (artifact_system.indexes)`:

```proto
message TodoList {
  option (artifact_system.indexes) = {
    key_type: "todo_list_by_name"
    key: ["name"]
    order: { field: "artifact_id" direction: ASCENDING }
    unique: true
  };

  string name = 1;
  string description = 2;
}
```

### Index Fields

| Field | Description |
|-------|-------------|
| `key_type` | Globally unique name for this index. Used when querying via `fetch_index`. |
| `key` | List of field paths that partition the index. All key fields are required when querying. An empty `key: []` creates a global index over all artifacts of this type. |
| `order` | List of `{ field, direction }` pairs defining sort order within each partition. `direction` must be `ASCENDING` or `DESCENDING`. |
| `unique` | When `true`, enforces at most one artifact per key value. |

### Index Rules

- The special field name `"artifact_id"` in order fields refers to the artifact's system-assigned ID.
- All key and order fields must be scalar or enum types (not messages or maps).
- `optional` fields: if unset, the artifact produces no index entry for that index.
- Implicit-presence scalars: default values are indexed (e.g., `0` for integers, `""` for strings).
- `float`/`double` key fields: NaN values are rejected; negative zero is normalized to positive zero.
- Dotted field paths (e.g., `"nested.field"`) are supported for nested message fields.
- A type can have multiple indexes.

### Repeated Field Indexing

Key fields may reference repeated scalar fields and fields nested within repeated messages. Repeated fields in an index must form a single linear ancestry chain — not a cartesian product across independent repeated paths.

**Supported patterns:**

| Pattern | Example | Rows produced |
|---------|---------|---------------|
| Repeated scalar leaf | `key: ["tags"]` | One row per tag value |
| Scalar within repeated message | `key: ["outputs.type_ref"]` | One row per output element |
| Nested repeated (repeated within repeated) | `key: ["inputs.types"]` | One row per type, per input element |

**Ancestry chain constraint:** All repeated fields across all key fields in an index must lie on a single root-to-leaf path. For example, `inputs` and `inputs.types` share the `inputs` ancestor and are valid together. But `inputs.name` and `outputs.type_ref` traverse different repeated messages and cannot appear as key fields in the same index.

**Path-correlated expansion:** Sibling fields within the same repeated message are correlated, not crossed. For the index `key: ["inputs.types", "is_view_capability"]` with order fields `inputs.name` and `inputs.required`, each emitted row binds `inputs.name` and `inputs.required` to the same `inputs` element that produced the `types` value.

**Virtual `_index` fields:** For any repeated field in the ancestry chain, `<path>._index` resolves to the 0-based position of the element in its parent array. It can be used in both key and order fields. For example, `inputs._index` gives the position in the `inputs` array, and `inputs.types._index` gives the position in the `types` array within each input. Virtual `_index` fields are typed as `uint32`.

**Order field constraints:** Order fields may reference sibling scalars at any repeated depth already expanded by key fields, `_index` for any repeated in the chain, root-level scalars, or `artifact_id`. Order fields **cannot** introduce new repeated paths not already expanded by key fields.

**Deduplication:** If multiple iterations produce identical key and order value tuples, duplicates are collapsed to a single entry.

**Example:**

```proto
message Input {
  string name = 1;
  bool required = 2;
  repeated uint64 types = 3;
}

message Action {
  option (artifact_system.indexes) = {
    key_type: "actions_by_input_type"
    key: ["inputs.types", "is_view_capability"]
    order: { field: "inputs._index" direction: ASCENDING }
    order: { field: "inputs.name" direction: ASCENDING }
    order: { field: "inputs.required" direction: ASCENDING }
    order: { field: "artifact_id" direction: ASCENDING }
  };

  repeated Input inputs = 1;
  bool is_view_capability = 2;
}
```

Given `inputs = [{name: "in1", required: true, types: [100, 200]}, {name: "in2", required: false, types: [300]}]` and `is_view_capability = true`, this produces three index rows:

| key (types, is_view) | order (_index, name, required, artifact_id) |
|---|---|
| (100, true) | (0, "in1", true, *aid*) |
| (200, true) | (0, "in1", true, *aid*) |
| (300, true) | (1, "in2", false, *aid*) |

## Defining References

References declare foreign-key relationships between artifact types. They are field-level options on `uint64` fields:

```proto
message TodoItem {
  option (artifact_system.indexes) = {
    key_type: "todo_items_by_list"
    key: ["list_id"]
    order: { field: "artifact_id" direction: ASCENDING }
  };

  uint64 list_id = 1 [(artifact_system.references) = {
    target_type_name: "todo.TodoList"
    on_delete: RESTRICT
  }];
  string title = 2;
  bool done = 3;
}
```

### `on_delete` Behaviors

| Value | Behavior |
|-------|----------|
| `RESTRICT` | Block deletion of the target if any artifact references it. |
| `CASCADE` | Recursively delete all referencing artifacts when the target is deleted. |
| `SET_NULL` | Clear the reference field. Only valid on `optional` or `repeated` fields. |

### Reference Rules

- Reference fields must be `uint64`, `optional uint64`, or `repeated uint64`.
- Each reference field requires exactly one covering index with that field as the sole key.
- On create/update, the referenced artifact must exist, not be deleted, and match the declared `target_type_name`.

---

## JavaScript Client

### Setup

```js
const { ArtifactClient } = require('artifact-client-js');
const grpc = require('@grpc/grpc-js');

const client = new ArtifactClient({
  service_address: '127.0.0.1:50051',
  channel_credentials: grpc.credentials.createInsecure(),
  retry: {               // optional
    max_retries: 5,       // default: 5
    base_delay_ms: 100,   // default: 100
    max_delay_ms: 10000,  // default: 10000
  },
});
await client.initialize();

// ... use client ...

client.close();
```

The client exposes two factory methods: `client.snapshot()` for reads and `client.transaction(callback)` for writes. There are no read or write methods directly on the client.

Transactions can optionally fork from a specific snapshot rather than the canonical branch head:

```js
await client.transaction(callback, { parent_snapshot_id: snapshot_id });
```

### Registering Types

Pass the `.proto` source as a string. Registration returns a `version_id` needed for all subsequent create/update operations.

```js
const todo_list_proto = fs.readFileSync('todo_list.proto', 'utf8');
const todo_item_proto = fs.readFileSync('todo_item.proto', 'utf8');

const result = await client.transaction(async (txn) => {
  const list_reg = await txn.register_type('todo.TodoList', todo_list_proto);
  const item_reg = await txn.register_type('todo.TodoItem', todo_item_proto);
  return {
    list_version_id: list_reg.version_id,
    item_version_id: item_reg.version_id,
  };
});

const list_version_id = result.value.list_version_id;
const item_version_id = result.value.item_version_id;
```

Optional mutation restriction flags can be passed:
```js
await txn.register_type('todo.TodoList', proto_source, {
  deny_create: false,
  deny_update: false,
  deny_delete: false,
});
```

### Creating Artifacts

Pass the `version_id` and a plain JS object matching the proto schema. The client handles protobuf encoding automatically.

```js
const result = await client.transaction(async (txn) => {
  const created = await txn.create(list_version_id, {
    name: 'Groceries',
    description: 'Weekly shopping list',
  });
  return created.artifact_id;
});

const list_id = result.value;
```

The transaction callback's return value is available as `result.value`. A `result.snapshot_id` is also returned for read-after-write.

### Reading Artifacts

Reads go through snapshots, which provide a consistent point-in-time view.

```js
const snapshot = await client.snapshot();

// Single artifact
const artifact = await snapshot.get(artifact_id);
// => { artifact_id, type_name, version_id, payload }
// payload is a decoded JS object, e.g. { name: 'Groceries', description: '...' }

// Multiple artifacts
const results = await snapshot.batch_get([id_1, id_2, id_3]);
// => Array of artifact objects, or null for missing IDs
```

Reads inside a transaction see the transaction's own writes:

```js
await client.transaction(async (txn) => {
  const created = await txn.create(version_id, { name: 'Test' });
  const fetched = await txn.get(created.artifact_id); // sees the write above
});
```

### Querying via Indexes

Use `fetch_index` with the `key_type` and a key object matching the index's key fields:

```js
const snapshot = await client.snapshot();
const result = await snapshot.fetch_index('todo_items_by_list', {
  list_id: list_id,
});

// result.index_payload.value.artifact_id is an array of matching artifact IDs
const item_ids = result.index_payload.value.artifact_id;

// Fetch the actual artifacts
const items = await snapshot.batch_get(item_ids);
```

The typical query pattern is: `fetch_index` to get artifact IDs, then `batch_get` to retrieve payloads.

### Updating Artifacts

Updates replace the full payload -- partial updates are not supported. Pass the `artifact_id`, `version_id`, and the complete new payload:

```js
await client.transaction(async (txn) => {
  await txn.update(item_id, item_version_id, {
    list_id: list_id,
    title: 'Milk',
    done: true,
  });
});
```

### Deleting Artifacts

```js
await client.transaction(async (txn) => {
  await txn.delete(item_id);
});
```

Deletes are logical tombstones. All associated index entries are automatically removed. Referential integrity is enforced (see [Defining References](#defining-references)).

### Nested Transactions

Transactions can be nested. A sub-transaction commits into the parent on success and rolls back on failure without affecting the parent:

```js
await client.transaction(async (txn) => {
  await txn.transaction(async (subtxn) => {
    await subtxn.create(version_id, { name: 'Nested' });
  });
  // sub-transaction's writes are now visible in the parent
});
```

### Transaction Snapshots

A snapshot can be forked from a transaction's current state. These snapshots remain usable even after the transaction settles:

```js
await client.transaction(async (txn) => {
  const created = await txn.create(version_id, { name: 'Example' });
  const snap = await txn.snapshot(); // captures the transaction's in-progress state
  const artifact = await snap.get(created.artifact_id);
});
```

### Type and Index Introspection

These read methods are available on both snapshots and transactions:

```js
const snapshot = await client.snapshot();

// Get metadata for a specific type version
const version = await snapshot.get_type_version(version_id);

// List all version IDs for a type (in registration order)
const version_ids = await snapshot.list_type_versions('todo.TodoList');

// Get the index schema (key fields, order fields, generated proto types)
const schema = await snapshot.get_index_schema('todo_items_by_list');
```

### Error Handling

All errors except `TransactionSettledError` and `TypeDecodeError` extend `ArtifactError`, which carries a `code` (gRPC status code), `detail` (parsed error info), and `grpc_error` (original error). `TransactionSettledError` and `TypeDecodeError` extend plain `Error` and do not carry those properties.

```js
const {
  ArtifactNotFoundError,
  WriteValidationError,
  ConflictError,
  TransactionError,
  TransactionSettledError,
  IndexFetchError,
  TypeRegistrationError,
  TypeDecodeError,
} = require('artifact-client-js');
```

| Error | When Thrown |
|-------|------------|
| `ArtifactNotFoundError` | `get()` for a missing or tombstoned artifact. `detail.tombstoned` indicates if it was deleted. |
| `WriteValidationError` | Create/update with invalid payload, empty payload, or denied mutation. `detail.violations` lists issues. |
| `ConflictError` | Commit conflict (concurrent writes, unique index violation). `detail.retryable` indicates if retry is safe; `detail.conflict_type` identifies the conflict category. |
| `TransactionError` | Transaction not found or expired. |
| `TransactionSettledError` | Using a transaction reference after its callback has completed. |
| `IndexFetchError` | `fetch_index()` failures: unknown index key type, incomplete key (missing required key fields), or key parse failure. |
| `TypeRegistrationError` | Invalid proto source or incompatible schema change. `detail.violations` lists issues. |
| `TypeDecodeError` | Proto descriptor or type resolution failure. |

```js
try {
  await snapshot.get('999');
} catch (err) {
  if (err instanceof ArtifactNotFoundError) {
    console.log('Tombstoned:', err.detail.tombstoned);
  }
}
```

**Transaction error behavior**: If the callback throws, the transaction auto-rolls back and the error is re-thrown. If rollback itself also fails, both errors are wrapped in an `AggregateError`.

---

## Constraints and Gotchas

- **All IDs are strings in JS.** Artifact IDs, version IDs, and type IDs are `uint64` internally, which exceeds JavaScript's safe integer range. The JS client represents all IDs as strings.
- **Updates are full replacements.** Every update writes the complete payload. There are no partial/patch updates.
- **No query-time filtering or pagination.** `fetch_index` returns the full stored index for a key. There is no cursor, limit, offset, or where-clause filtering.
- **No backfill for new indexes.** Indexes added in a new type version only cover artifacts written after registration. Historical artifacts are not retroactively indexed.
- **Snapshots are long-lived.** Once created, a snapshot remains usable indefinitely. Snapshots created from a transaction (`txn.snapshot()`) outlive the transaction.
- **Transactions auto-settle.** After the callback completes, all methods on the transaction object throw `TransactionSettledError`.
- **Proto3 only.** Only `proto3` syntax is accepted for type definitions.
- **`map` fields are not indexable.** Use `repeated` message fields for indexable map-like data.
- **Mutation restriction flags are tighten-only.** Once `deny_create`, `deny_update`, or `deny_delete` is set to `true`, it cannot be changed back to `false`.
- **Conflict handling.** On `ConflictError`, create a new transaction, re-read state, and retry. Check `err.detail.retryable` -- non-retryable conflicts (e.g., unique index violations) require application-level resolution.

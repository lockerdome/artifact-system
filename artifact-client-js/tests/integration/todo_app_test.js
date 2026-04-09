"use strict";

const fs = require('fs');
const path = require('path');
const { describe, it, before, after } = require('node:test');
const assert = require('node:assert');
const grpc = require('@grpc/grpc-js');
const {
  ArtifactClient,
  ArtifactNotFoundError,
  ConflictError,
} = require('../../lib/index');
const { start_server } = require('./server_harness');

// ─────────────────────────────────────────────────────────────────────────────
// Fixture proto sources — read from disk and passed to register_type()
// ─────────────────────────────────────────────────────────────────────────────

const FIXTURES_DIR = path.resolve(__dirname, 'fixtures');
const TODO_LIST_PROTO = fs.readFileSync(path.join(FIXTURES_DIR, 'todo_list.proto'), 'utf8');
const TODO_ITEM_PROTO = fs.readFileSync(path.join(FIXTURES_DIR, 'todo_item.proto'), 'utf8');

// ─────────────────────────────────────────────────────────────────────────────
// Integration test: Todo App
//
// Exercises the full client ↔ server round-trip:
//   - Type registration (with custom indexes and references)
//   - Artifact create / get / update / delete
//   - Index lookup (fetch_index)
//   - Referential integrity enforcement (RESTRICT on delete)
//   - Unique index enforcement
//   - Payload decode/encode through real protoc-emitted descriptor sets
// ─────────────────────────────────────────────────────────────────────────────

describe('Integration: Todo App', () => {
  let server;
  let client;

  // Populated during type registration
  let list_version_id;
  let item_version_id;

  before(async () => {
    server = await start_server();
    client = new ArtifactClient({
      service_address: server.address,
      channel_credentials: grpc.credentials.createInsecure(),
    });
    await client.initialize();
  });

  after(async () => {
    client.close();
    await server.stop();
  });

  // ─── 1. Bootstrap: register both types ──────────────────────────────────

  it('registers TodoList and TodoItem types', async () => {
    const result = await client.transaction(async (txn) => {
      const list_reg = await txn.register_type('todo.TodoList', TODO_LIST_PROTO);
      const item_reg = await txn.register_type('todo.TodoItem', TODO_ITEM_PROTO);
      return { list_version_id: list_reg.version_id, item_version_id: item_reg.version_id };
    });

    list_version_id = result.value.list_version_id;
    item_version_id = result.value.item_version_id;

    assert.ok(list_version_id, 'list_version_id should be set');
    assert.ok(item_version_id, 'item_version_id should be set');
  });

  // ─── 2. Create + index lookup ──────────────────────────────────────────

  let list_id;
  let item_ids = [];

  it('creates a list and items, then fetches items via index', async () => {
    // Create a TodoList
    const list_result = await client.transaction(async (txn) => {
      const created = await txn.create(list_version_id, {
        name: 'Groceries',
        description: 'Weekly shopping list',
      });
      return created.artifact_id;
    });
    list_id = list_result.value;
    assert.ok(list_id, 'list_id should be set');

    // Create three TodoItems referencing the list
    const items_result = await client.transaction(async (txn) => {
      const ids = [];
      for (const title of ['Milk', 'Eggs', 'Bread']) {
        const created = await txn.create(item_version_id, {
          list_id,
          title,
          done: false,
        });
        ids.push(created.artifact_id);
      }
      return ids;
    });
    item_ids = items_result.value;
    assert.strictEqual(item_ids.length, 3);

    // Fetch items via the todo_items_by_list index
    const snapshot = await client.snapshot();
    const index_result = await snapshot.fetch_index('todo_items_by_list', {
      list_id,
    });

    assert.ok(index_result.index_payload, 'index_payload should be set');
    assert.ok(index_result.index_payload.value, 'index_payload.value should be set');
    assert.strictEqual(index_result.index_payload.value.artifact_id.length, 3,
      'should find 3 items in the index');
  });

  // ─── 3. Update: mark an item as done ────────────────────────────────────

  it('updates an item and verifies the change via get', async () => {
    const target_id = item_ids[0];

    await client.transaction(async (txn) => {
      await txn.update(target_id, item_version_id, {
        list_id,
        title: 'Milk',
        done: true,
      });
    });

    const snapshot = await client.snapshot();
    const artifact = await snapshot.get(target_id);

    assert.strictEqual(artifact.payload.done, true, 'item should be marked done');
    assert.strictEqual(artifact.payload.title, 'Milk');
  });

  // ─── 4. Delete one item, verify index shrinks ──────────────────────────

  it('deletes an item and verifies index shrinks', async () => {
    const target_id = item_ids[2]; // "Bread"

    await client.transaction(async (txn) => {
      await txn.delete(target_id);
    });

    // Verify the deleted artifact is gone
    const snapshot = await client.snapshot();
    await assert.rejects(
      () => snapshot.get(target_id),
      (err) => {
        assert.ok(err instanceof ArtifactNotFoundError);
        return true;
      },
    );

    // Index should now have 2 entries
    const index_result = await snapshot.fetch_index('todo_items_by_list', {
      list_id,
    });
    assert.strictEqual(index_result.index_payload.value.artifact_id.length, 2,
      'should find 2 items after deletion');
  });

  // ─── 5. Referential integrity: can't delete list with items ────────────

  it('rejects deletion of list while items still reference it', async () => {
    await assert.rejects(
      () => client.transaction(async (txn) => {
        await txn.delete(list_id);
      }),
      (err) => {
        // The server should reject this with some form of conflict or
        // write-validation error due to the RESTRICT reference.
        assert.ok(
          err.code != null,
          `expected a gRPC error with a code, got: ${err.name}: ${err.message}`,
        );
        return true;
      },
    );
  });

  // ─── 6. Successful tear-down: delete items, then list ──────────────────

  it('deletes remaining items, then the list', async () => {
    // Delete remaining items (item_ids[0] and item_ids[1])
    await client.transaction(async (txn) => {
      for (const id of item_ids.slice(0, 2)) {
        await txn.delete(id);
      }
    });

    // Now the list can be deleted
    await client.transaction(async (txn) => {
      await txn.delete(list_id);
    });

    // Verify the list is gone
    const snapshot = await client.snapshot();
    await assert.rejects(
      () => snapshot.get(list_id),
      (err) => {
        assert.ok(err instanceof ArtifactNotFoundError);
        return true;
      },
    );
  });

  // ─── 7. Unique index enforcement ───────────────────────────────────────

  it('rejects concurrent duplicate list names (unique index)', async () => {
    // Two transactions fork from the same base and both create the same
    // unique-key value.  The first commit succeeds; the second is
    // rejected as a non-retryable conflict during the 3-way merge.
    //
    // We use the raw gRPC client to open two transactions explicitly
    // (the high-level client.transaction() auto-commits, which makes
    // real concurrency awkward).
    const grpc_client = client._grpc_client;
    const type_registry = client._type_registry;

    const tx1 = await grpc_client.create_transaction({});
    const tx2 = await grpc_client.create_transaction({});

    // Both transactions create a list with the same name.
    const read_ctx = { transaction_id: tx1.transaction_id };
    const { payload_bytes } = await type_registry.encode_artifact_payload(
      list_version_id,
      { name: 'Concurrent Dup', description: 'tx1' },
      read_ctx,
    );

    await grpc_client.create_artifact({
      version_id: list_version_id,
      payload: payload_bytes,
      transaction_id: tx1.transaction_id,
    });
    await grpc_client.create_artifact({
      version_id: list_version_id,
      payload: payload_bytes,
      transaction_id: tx2.transaction_id,
    });

    // First commit succeeds.
    await grpc_client.commit_transaction({
      transaction_id: tx1.transaction_id,
    });

    // Second commit should fail: the unique index key now has concurrent
    // modifications from both branches, and the conflict resolver
    // classifies unique index conflicts as non-retryable.
    await assert.rejects(
      () => grpc_client.commit_transaction({
        transaction_id: tx2.transaction_id,
      }),
      (err) => {
        assert.ok(
          err instanceof ConflictError,
          `expected ConflictError, got: ${err.name}: ${err.message}`,
        );
        assert.strictEqual(err.detail.retryable, false);
        return true;
      },
    );
  });

  // ─── 8. Sequential unique index enforcement ─────────────────────────────

  it('rejects sequential duplicate list names (unique index)', async () => {
    // Unlike the concurrent case above, this tests that the server also
    // rejects a duplicate unique-key value when the second create happens
    // in a transaction that forked *after* the first committed — i.e.
    // there is no concurrent modification, just a write that would produce
    // two entries under the same unique key.
    await client.transaction(async (txn) => {
      await txn.create(list_version_id, {
        name: 'Sequential Dup',
        description: 'first',
      });
    });

    await assert.rejects(
      () => client.transaction(async (txn) => {
        await txn.create(list_version_id, {
          name: 'Sequential Dup',
          description: 'second',
        });
      }),
      (err) => {
        assert.ok(
          err.code != null,
          `expected a gRPC error, got: ${err.name}: ${err.message}`,
        );
        return true;
      },
    );
  });
});

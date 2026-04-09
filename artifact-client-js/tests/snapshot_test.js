"use strict";

const { describe, it, before, after, beforeEach } = require('node:test');
const assert = require('node:assert');
const { ArtifactClient, ArtifactNotFoundError, IndexFetchError } = require('../lib/index');
const { MockArtifactServer } = require('./mock_server');

describe('Snapshot', () => {
  let mock_server;
  let client;

  before(async () => {
    mock_server = new MockArtifactServer();
    await mock_server.start();
    client = new ArtifactClient({ service_address: mock_server.address });
    await client.initialize();
  });

  after(async () => {
    client.close();
    await mock_server.stop();
  });

  beforeEach(() => {
    mock_server.reset();
  });

  describe('from canonical branch', () => {
    it('get() returns an artifact with decoded payload', async () => {
      mock_server.seed_artifact('10', { data: Buffer.from('payload'), label: 'hello' });
      const snapshot = await client.snapshot();
      const artifact = await snapshot.get('10');

      assert.strictEqual(artifact.artifact_id, '10');
      assert.strictEqual(artifact.type_name, mock_server.test_type_name);
      assert.strictEqual(artifact.version_id, mock_server.test_version_id);
      // Payload is decoded to a JS object, not raw bytes
      assert.strictEqual(artifact.payload.label, 'hello');
      assert.deepStrictEqual(Buffer.from(artifact.payload.data), Buffer.from('payload'));
    });

    it('get() throws ArtifactNotFoundError for missing artifact', async () => {
      const snapshot = await client.snapshot();
      await assert.rejects(
        () => snapshot.get('999'),
        (err) => {
          assert.ok(err instanceof ArtifactNotFoundError);
          assert.strictEqual(err.detail.tombstoned, false);
          return true;
        },
      );
    });

    it('batch_get() returns decoded artifacts and nulls', async () => {
      mock_server.seed_artifact('1', { data: Buffer.from('a'), label: 'a-label' });
      mock_server.seed_artifact('3', { data: Buffer.from('c'), label: 'c-label' });

      const snapshot = await client.snapshot();
      const results = await snapshot.batch_get(['1', '2', '3']);

      assert.strictEqual(results.length, 3);
      assert.strictEqual(results[0].artifact_id, '1');
      assert.strictEqual(results[0].payload.label, 'a-label');
      assert.strictEqual(results[1], null);
      assert.strictEqual(results[2].artifact_id, '3');
      assert.strictEqual(results[2].payload.label, 'c-label');
    });

    it('fetch_index() returns decoded index data', async () => {
      mock_server.seed_index(
        { id: 'k1' },
        { entries: [{ artifact_id: '42' }, { artifact_id: '43' }] },
      );

      const snapshot = await client.snapshot();
      const result = await snapshot.fetch_index(
        mock_server.test_index_key_type,
        { id: 'k1' },
      );

      assert.strictEqual(result.index_message_name, mock_server.test_index_message_name);
      assert.strictEqual(result.index_payload.entries.length, 2);
      assert.strictEqual(result.index_payload.entries[0].artifact_id, '42');
    });

    it('fetch_index() throws IndexFetchError for missing index', async () => {
      const snapshot = await client.snapshot();
      await assert.rejects(
        () => snapshot.fetch_index(mock_server.test_index_key_type, { id: 'missing' }),
        (err) => {
          assert.ok(err instanceof IndexFetchError);
          return true;
        },
      );
    });
  });

  describe('from transaction', () => {
    it('txn.snapshot() returns a usable Snapshot', async () => {
      mock_server.seed_artifact('20', { data: Buffer.from('x') });
      let txn_snapshot;

      await client.transaction(async (txn) => {
        txn_snapshot = await txn.snapshot();
      });

      // Snapshot remains usable after transaction settles
      const artifact = await txn_snapshot.get('20');
      assert.strictEqual(artifact.artifact_id, '20');
    });
  });

  describe('type registry reads', () => {
    it('get_type_version() returns version info', async () => {
      // Register a type first via transaction
      let registered_version_id;
      await client.transaction(async (txn) => {
        const result = await txn.register_type('test.RegType', 'syntax = "proto3";');
        registered_version_id = result.version_id;
      });

      const snapshot = await client.snapshot();
      const version = await snapshot.get_type_version(registered_version_id);
      assert.strictEqual(version.version_id, registered_version_id);
    });

    it('list_type_versions() returns version IDs', async () => {
      await client.transaction(async (txn) => {
        await txn.register_type('test.ListType', 'syntax = "proto3";');
      });

      const snapshot = await client.snapshot();
      const versions = await snapshot.list_type_versions('test.ListType');
      assert.ok(Array.isArray(versions));
      assert.strictEqual(versions.length, 1);
    });

    it('get_index_schema() returns schema', async () => {
      const snapshot = await client.snapshot();
      const schema = await snapshot.get_index_schema('some_key_type');
      assert.strictEqual(schema.key_type, 'some_key_type');
      assert.ok(Array.isArray(schema.key_fields));
    });
  });

  describe('snapshot properties', () => {
    it('does not expose snapshot_id via a public id field', async () => {
      const snapshot = await client.snapshot();
      assert.strictEqual(snapshot.id, undefined);
      assert.strictEqual(snapshot.snapshot_id, undefined);
    });

    it('remains usable indefinitely after creation', async () => {
      mock_server.seed_artifact('30', { data: Buffer.from('y') });
      const snapshot = await client.snapshot();

      // Use it multiple times
      const a1 = await snapshot.get('30');
      const a2 = await snapshot.get('30');
      assert.strictEqual(a1.artifact_id, a2.artifact_id);
    });
  });
});

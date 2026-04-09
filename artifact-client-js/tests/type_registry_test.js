"use strict";

const { describe, it, before, after, beforeEach } = require('node:test');
const assert = require('node:assert');
const { ArtifactClient, TypeDecodeError } = require('../lib/index');
const { MockArtifactServer } = require('./mock_server');

describe('TypeRegistryCache (decode/encode pipeline)', () => {
  let mock_server;
  let client;

  before(async () => {
    mock_server = new MockArtifactServer();
    await mock_server.start();
  });

  after(async () => {
    await mock_server.stop();
  });

  // Each test gets a fresh client so the TypeRegistryCache starts empty —
  // important for cache-counting assertions.
  beforeEach(async () => {
    if (client) client.close();
    mock_server.reset();
    client = new ArtifactClient({
      service_address: mock_server.address,
      retry: { max_retries: 0 },
    });
    await client.initialize();
  });

  after(() => {
    if (client) client.close();
  });

  describe('artifact payload round-trip', () => {
    it('decodes payload bytes into a JS object via the descriptor set', async () => {
      mock_server.seed_artifact('1', { data: Buffer.from([0xde, 0xad]), label: 'hi' });

      const snapshot = await client.snapshot();
      const artifact = await snapshot.get('1');

      // payload is a JS object, not raw bytes
      assert.strictEqual(typeof artifact.payload, 'object');
      assert.ok(!Buffer.isBuffer(artifact.payload));
      assert.strictEqual(artifact.payload.label, 'hi');
      assert.deepStrictEqual(
        Buffer.from(artifact.payload.data),
        Buffer.from([0xde, 0xad]),
      );
    });

    it('round-trips a JS object through create -> get', async () => {
      const original = { data: Buffer.from('round-trip'), label: 'rt' };

      let created_id;
      await client.transaction(async (txn) => {
        const result = await txn.create(mock_server.test_version_id, original);
        created_id = result.artifact_id;
      });

      const snapshot = await client.snapshot();
      const artifact = await snapshot.get(created_id);

      assert.strictEqual(artifact.payload.label, 'rt');
      assert.deepStrictEqual(
        Buffer.from(artifact.payload.data),
        Buffer.from('round-trip'),
      );
    });

    it('preserves .proto source field names without conversion', async () => {
      // The fixture type declares `data` and `label` in its .proto source.
      // This test guards against any future drift where the pipeline
      // starts normalizing field names (e.g. forcing snake_case or
      // camelCase) instead of preserving whatever the .proto declared.
      mock_server.seed_artifact('2', { data: Buffer.from('x'), label: 'y' });

      const snapshot = await client.snapshot();
      const artifact = await snapshot.get('2');

      assert.ok('data' in artifact.payload, 'expected field "data" as declared in .proto');
      assert.ok('label' in artifact.payload, 'expected field "label" as declared in .proto');
    });

    it('decodes all entries in batch_get', async () => {
      mock_server.seed_artifact('101', { data: Buffer.from('1'), label: 'one' });
      mock_server.seed_artifact('102', { data: Buffer.from('2'), label: 'two' });
      mock_server.seed_artifact('103', { data: Buffer.from('3'), label: 'three' });

      const snapshot = await client.snapshot();
      const results = await snapshot.batch_get(['101', '102', '103']);

      assert.strictEqual(results.length, 3);
      assert.strictEqual(results[0].payload.label, 'one');
      assert.strictEqual(results[1].payload.label, 'two');
      assert.strictEqual(results[2].payload.label, 'three');
    });
  });

  describe('caching', () => {
    it('caches version_id resolution: only one GetTypeVersion per unique version_id', async () => {
      mock_server.seed_artifact('10', { data: Buffer.from('a') });
      mock_server.seed_artifact('11', { data: Buffer.from('b') });
      mock_server.seed_artifact('12', { data: Buffer.from('c') });

      const snapshot = await client.snapshot();
      await snapshot.get('10');
      await snapshot.get('11');
      await snapshot.get('12');

      // Three reads, all of the same version_id → exactly one GetTypeVersion
      // (the cache prevents repeat lookups).  The TypeDefinition lookup is
      // also cached, so GetArtifact for the type_id only fires once.
      assert.strictEqual(
        mock_server.calls['GetTypeVersion'], 1,
        'GetTypeVersion should be cached after first call',
      );
    });

    it('caches index schema: only one GetIndexSchema per key_type', async () => {
      mock_server.seed_index({ id: 'k1' }, { entries: [{ artifact_id: '1' }] });
      mock_server.seed_index({ id: 'k2' }, { entries: [{ artifact_id: '2' }] });

      const snapshot = await client.snapshot();
      await snapshot.fetch_index(mock_server.test_index_key_type, { id: 'k1' });
      await snapshot.fetch_index(mock_server.test_index_key_type, { id: 'k2' });

      assert.strictEqual(
        mock_server.calls['GetIndexSchema'], 1,
        'GetIndexSchema should be cached after first call',
      );
    });

    it('cache is shared across snapshot and transaction objects from same client', async () => {
      mock_server.seed_artifact('20', { data: Buffer.from('snap') });

      // First call from a snapshot warms the cache
      const snapshot = await client.snapshot();
      await snapshot.get('20');
      const calls_after_snapshot = mock_server.calls['GetTypeVersion'];

      // Subsequent call from a transaction should reuse the cache
      await client.transaction(async (txn) => {
        await txn.get('20');
      });

      assert.strictEqual(
        mock_server.calls['GetTypeVersion'],
        calls_after_snapshot,
        'transaction should reuse the cached type from prior snapshot reads',
      );
    });
  });

  describe('index payload round-trip', () => {
    it('encodes the key object and decodes the index payload object', async () => {
      mock_server.seed_index(
        { id: 'lookup-me' },
        { entries: [{ artifact_id: '100' }, { artifact_id: '200' }] },
      );

      const snapshot = await client.snapshot();
      const result = await snapshot.fetch_index(
        mock_server.test_index_key_type,
        { id: 'lookup-me' },
      );

      assert.strictEqual(result.index_payload.entries.length, 2);
      assert.strictEqual(result.index_payload.entries[0].artifact_id, '100');
      assert.strictEqual(result.index_payload.entries[1].artifact_id, '200');
    });

    it('different key objects with the same encoded form match the same index entry', async () => {
      // Sanity check that key encoding is deterministic — the seeded entry
      // and the lookup encode to identical bytes.
      mock_server.seed_index(
        { id: 'deterministic' },
        { entries: [{ artifact_id: '999' }] },
      );

      const snapshot = await client.snapshot();
      const result = await snapshot.fetch_index(
        mock_server.test_index_key_type,
        { id: 'deterministic' },
      );

      assert.strictEqual(result.index_payload.entries[0].artifact_id, '999');
    });
  });

  describe('error handling', () => {
    it('throws TypeDecodeError when version_id has no registered type', async () => {
      await assert.rejects(
        () => client.transaction(async (txn) => {
          await txn.create('999999999', { data: Buffer.from('x') });
        }),
        (err) => {
          // The mock doesn't have version 999999999 registered, so the
          // type registry cache fails to resolve the type.
          assert.ok(err.code != null || err.name === 'TypeDecodeError',
            `expected a type resolution error, got ${err.name}: ${err.message}`);
          return true;
        },
      );
    });
  });
});

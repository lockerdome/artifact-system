"use strict";

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert');
const { ArtifactClient, TransactionSettledError } = require('../lib/index');
const { MockArtifactServer } = require('./mock_server');

describe('ArtifactClient', () => {
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

  it('requires service_address', () => {
    assert.throws(() => new ArtifactClient({}), /service_address is required/);
    assert.throws(() => new ArtifactClient(), /service_address is required/);
  });

  it('snapshot() returns a working Snapshot', async () => {
    mock_server.seed_artifact('100', 'test.Type', '1', Buffer.from('hello'));

    const snapshot = await client.snapshot();
    const artifact = await snapshot.get('100');
    assert.strictEqual(artifact.artifact_id, '100');
    assert.strictEqual(artifact.type_name, 'test.Type');
  });

  it('does not expose read/write/registry methods', () => {
    const forbidden = [
      'get', 'batch_get', 'create', 'update', 'delete',
      'register_type', 'get_type_version', 'list_type_versions',
      'get_index_schema', 'fetch_index',
    ];
    for (const method of forbidden) {
      assert.strictEqual(typeof client[method], 'undefined', `client should not have ${method}`);
    }
  });

  it('transaction() auto-commits on success', async () => {
    const result = await client.transaction(async (txn) => {
      const created = await txn.create('1', Buffer.from('data'));
      return created.artifact_id;
    });

    assert.ok(result.snapshot_id);
    assert.ok(result.value);
  });

  it('transaction() auto-rolls back on failure', async () => {
    await assert.rejects(async () => {
      await client.transaction(async () => {
        throw new Error('intentional failure');
      });
    }, (err) => {
      assert.strictEqual(err.message, 'intentional failure');
      return true;
    });

    assert.ok(mock_server.calls['RollbackTransaction'] > 0);
  });
});

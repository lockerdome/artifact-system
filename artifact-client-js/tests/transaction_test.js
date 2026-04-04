"use strict";

const { describe, it, before, after, beforeEach } = require('node:test');
const assert = require('node:assert');
const grpc = require('@grpc/grpc-js');
const {
  ArtifactClient,
  TransactionSettledError,
  ConflictError,
} = require('../lib/index');
const { MockArtifactServer, make_grpc_error_with_detail } = require('./mock_server');

describe('Transaction', () => {
  let mock_server;
  let client;

  before(async () => {
    mock_server = new MockArtifactServer();
    await mock_server.start();
    client = new ArtifactClient({
      service_address: mock_server.address,
      retry: { max_retries: 0 },
    });
    await client.initialize();
  });

  after(async () => {
    client.close();
    await mock_server.stop();
  });

  beforeEach(() => {
    mock_server.reset();
  });

  describe('auto-commit on success', () => {
    it('commits and returns snapshot_id + value', async () => {
      const result = await client.transaction(async (txn) => {
        const created = await txn.create('1', Buffer.from('data'));
        return created.artifact_id;
      });

      assert.ok(result.snapshot_id);
      assert.ok(result.value);
      assert.ok(mock_server.calls['CommitTransaction'] >= 1);
    });
  });

  describe('auto-rollback on failure', () => {
    it('rolls back and re-throws on callback throw', async () => {
      const err_msg = 'intentional error';
      await assert.rejects(
        () => client.transaction(async () => { throw new Error(err_msg); }),
        (err) => {
          assert.strictEqual(err.message, err_msg);
          return true;
        },
      );
      assert.ok(mock_server.calls['RollbackTransaction'] >= 1);
      assert.strictEqual(mock_server.calls['CommitTransaction'], undefined);
    });
  });

  describe('return value passthrough', () => {
    it('passes callback return value through result.value', async () => {
      const result = await client.transaction(async () => {
        return { answer: 42 };
      });
      assert.deepStrictEqual(result.value, { answer: 42 });
    });

    it('passes undefined when callback returns nothing', async () => {
      const result = await client.transaction(async () => {});
      assert.strictEqual(result.value, undefined);
    });
  });

  describe('reads within transaction', () => {
    it('get() works inside transaction', async () => {
      mock_server.seed_artifact('50', 'test.T', '1', Buffer.from('x'));

      await client.transaction(async (txn) => {
        const artifact = await txn.get('50');
        assert.strictEqual(artifact.artifact_id, '50');
      });
    });

    it('batch_get() works inside transaction', async () => {
      mock_server.seed_artifact('60', 'test.T', '1', Buffer.from('a'));

      await client.transaction(async (txn) => {
        const results = await txn.batch_get(['60', '61']);
        assert.strictEqual(results[0].artifact_id, '60');
        assert.strictEqual(results[1], null);
      });
    });

    it('fetch_index() works inside transaction', async () => {
      const key = Buffer.from([5, 6]);
      mock_server.seed_index('idx', key, Buffer.from('val'), 'Index_Foo');

      await client.transaction(async (txn) => {
        const result = await txn.fetch_index('idx', key);
        assert.strictEqual(result.index_message_name, 'Index_Foo');
      });
    });
  });

  describe('writes within transaction', () => {
    it('create() returns artifact_id and snapshot_id', async () => {
      await client.transaction(async (txn) => {
        const created = await txn.create('1', Buffer.from('payload'));
        assert.ok(created.artifact_id);
        assert.ok(created.snapshot_id);
      });
    });

    it('update() returns snapshot_id', async () => {
      mock_server.seed_artifact('70', 'test.T', '1', Buffer.from('old'));

      await client.transaction(async (txn) => {
        const result = await txn.update('70', '2', Buffer.from('new'));
        assert.ok(result.snapshot_id);
      });
    });

    it('delete() returns snapshot_id', async () => {
      mock_server.seed_artifact('80', 'test.T', '1', Buffer.from('x'));

      await client.transaction(async (txn) => {
        const result = await txn.delete('80');
        assert.ok(result.snapshot_id);
      });
    });
  });

  describe('type registry in transaction', () => {
    it('register_type() returns version_id', async () => {
      await client.transaction(async (txn) => {
        const result = await txn.register_type('test.NewType', 'syntax = "proto3";');
        assert.ok(result.version_id);
      });
    });

    it('readable registry methods work on txn', async () => {
      await client.transaction(async (txn) => {
        const reg = await txn.register_type('test.ReadReg', 'syntax = "proto3";');

        const version = await txn.get_type_version(reg.version_id);
        assert.strictEqual(version.version_id, reg.version_id);

        const versions = await txn.list_type_versions('test.ReadReg');
        assert.ok(versions.length >= 1);

        const schema = await txn.get_index_schema('some_key');
        assert.ok(schema.key_type);
      });
    });
  });

  describe('nested transactions', () => {
    it('sub-transaction commits into parent', async () => {
      const result = await client.transaction(async (txn) => {
        const sub_result = await txn.transaction(async (subtxn) => {
          const created = await subtxn.create('1', Buffer.from('sub'));
          return created.artifact_id;
        });
        assert.ok(sub_result.snapshot_id);
        assert.ok(sub_result.value);
        return 'parent_done';
      });

      assert.strictEqual(result.value, 'parent_done');
    });

    it('sub-transaction rollback does not affect parent', async () => {
      const result = await client.transaction(async (txn) => {
        try {
          await txn.transaction(async () => {
            throw new Error('sub fails');
          });
        } catch (err) {
          assert.strictEqual(err.message, 'sub fails');
        }
        return 'parent_ok';
      });

      assert.strictEqual(result.value, 'parent_ok');
    });
  });

  describe('ConflictError on commit', () => {
    it('throws ConflictError when commit fails with conflict', async () => {
      mock_server.set_failure('CommitTransaction', make_grpc_error_with_detail(
        grpc.status.ABORTED,
        'commit conflict',
        'artifact_system.CommitConflict',
        { conflict_type: 'INDEX_CONFLICT', retryable: true, attempts: 1 },
      ));

      await assert.rejects(
        () => client.transaction(async () => 'value'),
        (err) => {
          assert.ok(err instanceof ConflictError);
          assert.strictEqual(err.detail.conflict_type, 'INDEX_CONFLICT');
          return true;
        },
      );
    });
  });

  describe('rollback failure handling', () => {
    it('wraps both errors in AggregateError when rollback fails', async () => {
      mock_server.set_failure('RollbackTransaction', {
        code: grpc.status.INTERNAL,
        message: 'rollback failed',
      });

      await assert.rejects(
        () => client.transaction(async () => { throw new Error('callback error'); }),
        (err) => {
          assert.ok(err instanceof AggregateError);
          assert.strictEqual(err.errors.length, 2);
          assert.strictEqual(err.errors[0].message, 'callback error');
          return true;
        },
      );
    });
  });

  describe('settled enforcement', () => {
    it('all methods throw TransactionSettledError after callback settles', async () => {
      let leaked_txn;

      await client.transaction(async (txn) => {
        leaked_txn = txn;
      });

      const methods = [
        () => leaked_txn.get('1'),
        () => leaked_txn.batch_get(['1']),
        () => leaked_txn.fetch_index('k', Buffer.from([1])),
        () => leaked_txn.create('1', Buffer.from('x')),
        () => leaked_txn.update('1', '1', Buffer.from('x')),
        () => leaked_txn.delete('1'),
        () => leaked_txn.register_type('t', 'src'),
        () => leaked_txn.get_type_version('1'),
        () => leaked_txn.list_type_versions('t'),
        () => leaked_txn.get_index_schema('k'),
        () => leaked_txn.snapshot(),
        () => leaked_txn.transaction(async () => {}),
      ];

      for (const fn of methods) {
        await assert.rejects(fn, (err) => {
          assert.ok(err instanceof TransactionSettledError, `Expected TransactionSettledError, got ${err.name}`);
          return true;
        });
      }
    });

    it('snapshot created from txn remains usable after txn settles', async () => {
      mock_server.seed_artifact('90', 'test.T', '1', Buffer.from('data'));
      let txn_snapshot;

      await client.transaction(async (txn) => {
        txn_snapshot = await txn.snapshot();
      });

      // Snapshot is NOT settled — it should still work
      const artifact = await txn_snapshot.get('90');
      assert.strictEqual(artifact.artifact_id, '90');
    });
  });
});

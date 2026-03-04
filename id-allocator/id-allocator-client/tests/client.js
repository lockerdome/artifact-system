"use strict";

const assert = require('node:assert');
const { describe, it, before, after } = require('node:test');
const { IdAllocatorClient } = require('../lib/client');
const { MockAllocatorServer } = require('./mock_allocator_server');

describe('IdAllocatorClient', () => {
  describe('constructor validation', () => {
    it('throws without service_address', () => {
      assert.throws(
        () => new IdAllocatorClient({ partition_id: 'test' }),
        /service_address is required/,
      );
    });

    it('throws without partition_id', () => {
      assert.throws(
        () => new IdAllocatorClient({ service_address: 'localhost:50051' }),
        /partition_id is required/,
      );
    });
  });

  describe('with mock server', () => {
    let mock_server;

    before(async () => {
      mock_server = new MockAllocatorServer({ block_size: 50 });
      await mock_server.start();
    });

    after(async () => {
      await mock_server.stop();
    });

    it('initializes and allocates an ID', async () => {
      const client = new IdAllocatorClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
        high_water_mark: 10,
      });

      await client.initialize();

      const id = client.allocate_id();
      assert.strictEqual(typeof id, 'number');
      assert.ok(id >= 0);

      client.close();
    });

    it('allocates unique sequential IDs', async () => {
      mock_server.counter = 0;
      mock_server.call_count = 0;

      const client = new IdAllocatorClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
        high_water_mark: 10,
      });

      await client.initialize();

      const ids = [];
      for (let i = 0; i < 20; ++i) {
        ids.push(client.allocate_id());
      }

      assert.strictEqual(new Set(ids).size, 20);
      for (let i = 0; i < 20; ++i) {
        assert.strictEqual(ids[i], i);
      }

      client.close();
    });

    it('prefetches and swaps blocks across block boundaries', async () => {
      mock_server.counter = 0;
      mock_server.call_count = 0;

      const block_size = 50;
      const client = new IdAllocatorClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
        high_water_mark: 10,
      });

      await client.initialize();

      const ids = [];
      for (let i = 0; i < block_size + 20; ++i) {
        ids.push(client.allocate_id());
        // Let background prefetches settle
        await new Promise(resolve => setTimeout(resolve, 0));
      }

      assert.strictEqual(new Set(ids).size, block_size + 20);
      assert.ok(mock_server.call_count >= 2, 'Should have fetched at least 2 blocks');

      client.close();
    });

    it('throws when allocating before initialization', () => {
      const client = new IdAllocatorClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
      });

      assert.throws(
        () => client.allocate_id(),
        /Client is not initialized/,
      );
    });

    it('throws on double initialization', async () => {
      const client = new IdAllocatorClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
      });

      await client.initialize();
      await assert.rejects(
        () => client.initialize(),
        /Client is already initialized/,
      );

      client.close();
    });
  });
});

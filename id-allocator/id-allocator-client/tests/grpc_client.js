"use strict";

const assert = require('node:assert');
const { describe, it, before, after } = require('node:test');
const { IdAllocatorGrpcClient } = require('../lib/grpc_client');
const { MockAllocatorServer } = require('./mock_allocator_server');

/* Tests */

describe('IdAllocatorGrpcClient', () => {
  describe('constructor validation', () => {
    it('throws without service_address', () => {
      assert.throws(
        () => new IdAllocatorGrpcClient({ partition_id: 'test' }),
        /service_address is required/,
      );
    });

    it('throws without partition_id', () => {
      assert.throws(
        () => new IdAllocatorGrpcClient({ service_address: 'localhost:50051' }),
        /partition_id is required/,
      );
    });
  });

  it('throws when fetching before connect', () => {
    const client = new IdAllocatorGrpcClient({
      service_address: 'localhost:50051',
      partition_id: 'test_partition',
    });

    assert.throws(
      () => client.fetch_block(),
      /Client is not connected/,
    );
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

    it('fetches a block with correct range', async () => {
      mock_server.counter = 0;
      mock_server.call_count = 0;

      const client = new IdAllocatorGrpcClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
      });

      await client.connect();

      const block = await client.fetch_block();
      assert.strictEqual(block.range_start, 0);
      assert.strictEqual(block.range_end, 50);
      assert.strictEqual(mock_server.call_count, 1);

      client.close();
    });

    it('fetches sequential non-overlapping blocks', async () => {
      mock_server.counter = 0;
      mock_server.call_count = 0;

      const client = new IdAllocatorGrpcClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
      });

      await client.connect();

      const block1 = await client.fetch_block();
      const block2 = await client.fetch_block();

      assert.strictEqual(block1.range_start, 0);
      assert.strictEqual(block1.range_end, 50);
      assert.strictEqual(block2.range_start, 50);
      assert.strictEqual(block2.range_end, 100);

      client.close();
    });
  });

  describe('retry behavior', () => {
    it('retries transient failures', async () => {
      const mock_server = new MockAllocatorServer({
        block_size: 50,
        fail_until: 2,
      });
      await mock_server.start();

      const client = new IdAllocatorGrpcClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
        retry: {
          max_retries: 5,
          base_delay_ms: 10,
          max_delay_ms: 50,
        },
      });

      await client.connect();

      // Should succeed after retrying past the 2 failures
      const block = await client.fetch_block();
      assert.strictEqual(typeof block.range_start, 'number');
      assert.strictEqual(typeof block.range_end, 'number');
      assert.strictEqual(mock_server.call_count, 3);

      client.close();
      await mock_server.stop();
    });

    it('fails after exhausting retries', async () => {
      const mock_server = new MockAllocatorServer({
        block_size: 50,
        fail_until: 100,
      });
      await mock_server.start();

      const client = new IdAllocatorGrpcClient({
        service_address: mock_server.address,
        partition_id: 'test_partition',
        retry: {
          max_retries: 2,
          base_delay_ms: 10,
          max_delay_ms: 50,
        },
      });

      await client.connect();

      await assert.rejects(
        () => client.fetch_block(),
        /Mock transient failure/,
      );

      client.close();
      await mock_server.stop();
    });
  });
});

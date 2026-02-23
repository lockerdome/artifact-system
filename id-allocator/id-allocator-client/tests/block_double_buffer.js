"use strict";

const assert = require('node:assert');
const { describe, it, before, after } = require('node:test');
const { BlockDoubleBuffer } = require('../lib/block_double_buffer');

/* Helpers */

function make_fetch_block (block_size) {
  let counter = 0;
  return function fetch_block () {
    const range_start = counter;
    counter += block_size;
    return Promise.resolve({ range_start, range_end: counter });
  };
}

function make_delayed_fetch_block (block_size, delay_ms) {
  let counter = 0;
  return function fetch_block () {
    const range_start = counter;
    counter += block_size;
    return new Promise(resolve => {
      setTimeout(() => resolve({ range_start, range_end: counter }), delay_ms);
    });
  };
}

/* Suppress console.error from prefetch failure tests */

const original_console_error = console.error;
before(() => { console.error = () => {}; });
after(() => { console.error = original_console_error; });

/* Tests */

describe('BlockDoubleBuffer', () => {
  it('allocates IDs sequentially from the front block', async () => {
    const buffer = new BlockDoubleBuffer({
      high_water_mark: 2,
      fetch_block: make_fetch_block(10),
    });
    await buffer.initialize();

    assert.strictEqual(buffer.allocate_id(), 0);
    assert.strictEqual(buffer.allocate_id(), 1);
    assert.strictEqual(buffer.allocate_id(), 2);
  });

  it('triggers prefetch at high-water mark', async () => {
    let fetch_count = 0;
    const block_size = 10;
    let counter = 0;

    const buffer = new BlockDoubleBuffer({
      high_water_mark: 3,
      fetch_block: () => {
        fetch_count++;
        const range_start = counter;
        counter += block_size;
        return Promise.resolve({ range_start, range_end: counter });
      },
    });
    await buffer.initialize();
    assert.strictEqual(fetch_count, 1);

    // Allocate until high-water mark (remaining === 3 means we've allocated 7)
    for (let i = 0; i < 7; ++i) {
      buffer.allocate_id();
    }

    // Wait for the async prefetch to complete
    await buffer.fetch_promise;
    assert.strictEqual(fetch_count, 2);
  });

  it('swaps to back block when front is exhausted', async () => {
    const block_size = 5;
    const buffer = new BlockDoubleBuffer({
      high_water_mark: 2,
      fetch_block: make_fetch_block(block_size),
    });
    await buffer.initialize();

    // Exhaust front block (IDs 0..4), triggering prefetch at remaining===2
    for (let i = 0; i < block_size; ++i) {
      buffer.allocate_id();
    }

    // Wait for prefetch to complete
    if (buffer.fetch_promise) await buffer.fetch_promise;

    // Next allocation should come from the back block (now promoted to front)
    const id = buffer.allocate_id();
    assert.strictEqual(id, block_size); // should be 5
  });

  it('all allocated IDs are unique', async () => {
    const block_size = 10;
    const total = 50;
    const buffer = new BlockDoubleBuffer({
      high_water_mark: 3,
      fetch_block: make_fetch_block(block_size),
    });
    await buffer.initialize();

    const ids = new Set();
    for (let i = 0; i < total; ++i) {
      const id = buffer.allocate_id();
      assert.ok(!ids.has(id), `Duplicate ID: ${id}`);
      ids.add(id);

      // Let prefetches complete between blocks
      if (buffer.fetch_promise) await buffer.fetch_promise;
    }

    assert.strictEqual(ids.size, total);
  });

  it('throws when both buffers are exhausted', async () => {
    let fetch_count = 0;
    const buffer = new BlockDoubleBuffer({
      high_water_mark: 1,
      fetch_block: () => {
        fetch_count++;
        if (fetch_count > 1) {
          return Promise.reject(new Error('Service unavailable'));
        }
        return Promise.resolve({ range_start: 0, range_end: 3 });
      },
    });
    await buffer.initialize();

    // Allocate 2 (at remaining===1, prefetch fires and fails)
    buffer.allocate_id();
    buffer.allocate_id();

    // Let the failed prefetch settle
    await new Promise(resolve => setTimeout(resolve, 10));

    // Third allocation exhausts front
    buffer.allocate_id();

    // Fourth should throw — both blocks are empty
    assert.throws(
      () => buffer.allocate_id(),
      /ID pool depleted/,
    );
  });

  it('retries prefetch after a failure when below the high-water mark', async () => {
    let fetch_count = 0;
    const block_size = 6;
    let counter = 0;

    const buffer = new BlockDoubleBuffer({
      high_water_mark: 3,
      fetch_block: () => {
        fetch_count++;
        if (fetch_count === 2) {
          return Promise.reject(new Error('Service unavailable'));
        }
        const range_start = counter;
        counter += block_size;
        return Promise.resolve({ range_start, range_end: counter });
      },
    });
    await buffer.initialize();
    assert.strictEqual(fetch_count, 1);

    // Allocate to the high-water mark to trigger a failed prefetch
    for (let i = 0; i < 3; ++i) {
      buffer.allocate_id();
    }

    const failed_prefetch = buffer.fetch_promise;
    if (failed_prefetch) await failed_prefetch;
    assert.strictEqual(fetch_count, 2);

    // Allocate once more to drop below the mark and retry prefetch
    buffer.allocate_id();
    const retry_prefetch = buffer.fetch_promise;
    if (retry_prefetch) await retry_prefetch;

    assert.strictEqual(fetch_count, 3);
    assert.ok(buffer.back.has_id());
  });

  it('rejects invalid high_water_mark', () => {
    assert.throws(
      () => new BlockDoubleBuffer({ high_water_mark: 0, fetch_block: () => {} }),
      /high_water_mark must be a positive integer/,
    );
  });

  it('rejects missing fetch_block', () => {
    assert.throws(
      () => new BlockDoubleBuffer({ high_water_mark: 1 }),
      /fetch_block must be a function/,
    );
  });

  it('does not prefetch more than once concurrently', async () => {
    let fetch_count = 0;
    const block_size = 10;
    let counter = 0;

    const buffer = new BlockDoubleBuffer({
      high_water_mark: 5,
      fetch_block: () => {
        fetch_count++;
        const range_start = counter;
        counter += block_size;
        return new Promise(resolve => {
          setTimeout(() => resolve({ range_start, range_end: counter }), 50);
        });
      },
    });
    await buffer.initialize();
    assert.strictEqual(fetch_count, 1);

    // Allocate past the high-water mark multiple times
    for (let i = 0; i < 8; ++i) {
      buffer.allocate_id();
    }

    // Only one prefetch should have been triggered despite multiple calls
    // past the high-water mark
    assert.strictEqual(fetch_count, 2);

    if (buffer.fetch_promise) await buffer.fetch_promise;
  });

  it('handles delayed fetches without losing IDs', async () => {
    const block_size = 5;
    const buffer = new BlockDoubleBuffer({
      high_water_mark: 2,
      fetch_block: make_delayed_fetch_block(block_size, 20),
    });
    await buffer.initialize();

    const ids = [];
    for (let i = 0; i < 20; ++i) {
      ids.push(buffer.allocate_id());
      if (buffer.fetch_promise) await buffer.fetch_promise;
    }

    // All IDs should be unique
    assert.strictEqual(new Set(ids).size, 20);
  });
});

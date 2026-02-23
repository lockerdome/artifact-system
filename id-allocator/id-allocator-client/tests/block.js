"use strict";

const assert = require('node:assert');
const { describe, it } = require('node:test');
const { Block } = require('../lib/block');

describe('Block', () => {
  it('allocates IDs sequentially from range_start', () => {
    const block = new Block();
    block.fill(100, 105);

    assert.strictEqual(block.allocate_id(), 100);
    assert.strictEqual(block.allocate_id(), 101);
    assert.strictEqual(block.allocate_id(), 102);
    assert.strictEqual(block.allocate_id(), 103);
    assert.strictEqual(block.allocate_id(), 104);
  });

  it('reports remaining count correctly', () => {
    const block = new Block();
    block.fill(0, 10);

    assert.strictEqual(block.remaining(), 10);
    block.allocate_id();
    assert.strictEqual(block.remaining(), 9);
  });

  it('reports has_id correctly', () => {
    const block = new Block();
    assert.strictEqual(block.has_id(), false);

    block.fill(0, 1);
    assert.strictEqual(block.has_id(), true);

    block.allocate_id();
    assert.strictEqual(block.has_id(), false);
  });

  it('throws when allocating from an exhausted block', () => {
    const block = new Block();
    block.fill(0, 1);
    block.allocate_id();

    assert.throws(() => block.allocate_id(), /Block exhausted/);
  });

  it('throws when filling a block that still has IDs', () => {
    const block = new Block();
    block.fill(0, 10);

    assert.throws(() => block.fill(100, 200), /Cannot fill a block that still has IDs/);
  });

  it('can be reset and refilled', () => {
    const block = new Block();
    block.fill(0, 10);
    block.allocate_id();

    block.reset();
    assert.strictEqual(block.has_id(), false);
    assert.strictEqual(block.remaining(), 0);

    block.fill(500, 510);
    assert.strictEqual(block.allocate_id(), 500);
  });

  it('starts empty', () => {
    const block = new Block();
    assert.strictEqual(block.has_id(), false);
    assert.strictEqual(block.remaining(), 0);
  });
});

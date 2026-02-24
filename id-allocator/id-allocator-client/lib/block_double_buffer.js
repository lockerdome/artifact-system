"use strict";

const { Block } = require('./block');

/**
 * BlockDoubleBuffer maintains two Block buffers (front and back) and manages
 * prefetching from a remote source to keep IDs available.
 *
 * - IDs are allocated from the front block.
 * - When the front block reaches the high-water mark, a background fetch fills
 *   the back block.
 * - When the front block is exhausted, front and back are swapped.
 * - If both blocks are exhausted, allocate_id throws.
 *
 * The fetch_block callback is provided by the caller and returns a Promise
 * resolving to { range_start, range_end }.
 */
class BlockDoubleBuffer {
  /**
   * @param {object} options
   * @param {number} options.high_water_mark - When remaining IDs in the front
   *   block drops to this level, a prefetch of the back block is triggered.
   * @param {function} options.fetch_block - Async callback that fetches a new
   *   block from the service. Returns { range_start: number, range_end: number }.
   */
  constructor ({ high_water_mark, fetch_block }) {
    if (typeof high_water_mark !== 'number' || high_water_mark < 1) {
      throw new Error('high_water_mark must be a positive integer');
    }
    if (typeof fetch_block !== 'function') {
      throw new Error('fetch_block must be a function');
    }

    this.high_water_mark = high_water_mark;
    this.fetch_block = fetch_block;
    this.front = new Block();
    this.back = new Block();
    this.fetch_in_progress = false;
    this.fetch_promise = null;
  }

  /**
   * Fetches the initial front block. Must be called before allocate_id.
   */
  async initialize () {
    const { range_start, range_end } = await this.fetch_block();
    this.front.fill(range_start, range_end);
  }

  /**
   * Triggers a background fetch for the back block if one is not already
   * in progress and the back block is empty.
   */
  _maybe_prefetch () {
    if (this.fetch_in_progress || this.back.has_id()) {
      return;
    }

    this.fetch_in_progress = true;
    this.fetch_promise = this.fetch_block().then(({ range_start, range_end }) => {
      this.back.fill(range_start, range_end);
      this.fetch_in_progress = false;
      this.fetch_promise = null;
    }).catch((err) => {
      this.fetch_in_progress = false;
      this.fetch_promise = null;
      // The prefetch failed. This is not fatal — a subsequent allocate_id call
      // will re-trigger the prefetch if the front remains at or below the
      // high-water mark. The error is logged but not propagated since this is a
      // background operation.
      console.error('Failed to prefetch ID block:', err);
    });
  }

  /**
   * Swaps front and back blocks. The old front (now back) is reset.
   */
  _swap () {
    const temp = this.front;
    this.front = this.back;
    this.back = temp;
    this.back.reset();
  }

  /**
   * Allocates a single ID. This is synchronous in the common case (reading
   * from the local front buffer).
   *
   * Throws if both front and back blocks are exhausted.
   */
  allocate_id () {
    if (!this.front.has_id()) {
      if (!this.back.has_id()) {
        throw new Error('ID pool depleted: both front and back blocks are exhausted');
      }
      this._swap();
    }

    const id = this.front.allocate_id();

    if (this.front.remaining() <= this.high_water_mark) {
      this._maybe_prefetch();
    }

    return id;
  }
}

module.exports = { BlockDoubleBuffer };

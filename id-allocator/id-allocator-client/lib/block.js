"use strict";

/**
 * Block represents a contiguous range of IDs [range_start, range_end).
 * IDs are allocated sequentially from range_start.
 */
class Block {
  constructor () {
    this.range_start = 0;
    this.range_end = 0;
  }

  /**
   * Returns the number of remaining IDs in this block.
   */
  remaining () {
    return this.range_end - this.range_start;
  }

  /**
   * Returns true if this block has at least one ID available.
   */
  has_id () {
    return this.range_start < this.range_end;
  }

  /**
   * Allocates and returns the next ID from this block.
   * Throws if the block is exhausted.
   */
  allocate_id () {
    if (!this.has_id()) {
      throw new Error("Block exhausted");
    }
    return this.range_start++;
  }

  /**
   * Fills this block with a new range from the service.
   * The block must be empty before filling.
   */
  fill (range_start, range_end) {
    if (this.has_id()) {
      throw new Error("Cannot fill a block that still has IDs");
    }
    this.range_start = range_start;
    this.range_end = range_end;
  }

  /**
   * Discards any remaining IDs in this block.
   */
  reset () {
    this.range_start = 0;
    this.range_end = 0;
  }
}

module.exports = { Block };

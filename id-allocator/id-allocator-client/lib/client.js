"use strict";

const { BlockDoubleBuffer } = require('./block_double_buffer');
const { IdAllocatorGrpcClient } = require('./grpc_client');

const DEFAULT_HIGH_WATER_MARK = 1000;

/**
 * IdAllocatorClient provides a synchronous allocate_id() method backed by a
 * double-buffered local block cache.
 */
class IdAllocatorClient {
  /**
   * @param {object} options
   * @param {string} options.service_address - gRPC endpoint (host:port).
   * @param {string} options.partition_id - Partition to allocate from.
   * @param {number} [options.high_water_mark=1000] - Remaining IDs that triggers prefetch.
   * @param {object} [options.retry] - Retry options for gRPC calls.
   * @param {number} [options.retry.max_retries=5]
   * @param {number} [options.retry.base_delay_ms=100]
   * @param {number} [options.retry.max_delay_ms=10000]
   * @param {object} [options.channel_credentials] - gRPC channel credentials.
   *   Defaults to insecure credentials.
   */
  constructor (options) {
    if (!options || !options.service_address) {
      throw new Error('service_address is required');
    }
    if (!options.partition_id) {
      throw new Error('partition_id is required');
    }

    this._grpc_client = new IdAllocatorGrpcClient(options);
    this._buffer = new BlockDoubleBuffer({
      high_water_mark: options.high_water_mark ?? DEFAULT_HIGH_WATER_MARK,
      fetch_block: () => this._grpc_client.fetch_block(),
    });
    this._initialized = false;
  }

  /**
   * Connects to the gRPC service and fetches the initial block.
   * Must be called before allocate_id().
   */
  async initialize () {
    if (this._initialized) {
      throw new Error('Client is already initialized');
    }

    await this._grpc_client.connect();
    await this._buffer.initialize();
    this._initialized = true;
  }

  /**
   * Allocates a single ID. Synchronous — reads from the local buffer.
   * Throws if the client is not initialized or both buffers are exhausted.
   */
  allocate_id () {
    if (!this._initialized) {
      throw new Error('Client is not initialized. Call initialize() first.');
    }
    return this._buffer.allocate_id();
  }

  /**
   * Closes the gRPC channel. The client cannot be used after this.
   */
  close () {
    this._grpc_client.close();
    this._initialized = false;
  }
}

module.exports = { IdAllocatorClient };

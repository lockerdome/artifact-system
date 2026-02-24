"use strict";

const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const { retry_with_backoff } = require('./retry');

const PROTO_PATH = path.resolve(__dirname, '../../proto/id_allocator.proto');

const DEFAULT_RETRY_OPTIONS = {
  max_retries: 5,
  base_delay_ms: 100,
  max_delay_ms: 10000,
};

/**
 * IdAllocatorGrpcClient connects to the ID Allocator gRPC service and fetches
 * ID blocks.
 */
class IdAllocatorGrpcClient {
  /**
   * @param {object} options
   * @param {string} options.service_address - gRPC endpoint (host:port).
   * @param {string} options.partition_id - Partition to allocate from.
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

    this.service_address = options.service_address;
    this.partition_id = options.partition_id;
    this.retry_options = { ...DEFAULT_RETRY_OPTIONS, ...options.retry };
    this.channel_credentials = options.channel_credentials ?? grpc.credentials.createInsecure();

    this._grpc_client = null;
    this._connected = false;
  }

  /**
   * Connects to the gRPC service.
   */
  async connect () {
    if (this._connected) {
      return;
    }
    const package_definition = protoLoader.loadSync(PROTO_PATH, {
      keepCase: true,
      longs: Number,
      enums: String,
      defaults: true,
      oneofs: true,
    });
    const proto = grpc.loadPackageDefinition(package_definition);
    this._grpc_client = new proto.id_allocator.IdAllocator(
      this.service_address,
      this.channel_credentials,
    );

    this._connected = true;
  }

  /**
   * Closes the gRPC channel. The client cannot be used after this.
   */
  close () {
    if (this._grpc_client) {
      this._grpc_client.close();
      this._grpc_client = null;
    }
    this._connected = false;
  }

  /**
   * Fetches a block from the gRPC service with retry.
   * @returns {Promise<{range_start: number, range_end: number}>}
   */
  fetch_block () {
    if (!this._connected) {
      throw new Error('Client is not connected. Call connect() first.');
    }
    return retry_with_backoff(() => this._allocate_block_rpc(), this.retry_options);
  }

  /**
   * Makes a single AllocateBlock RPC call.
   * @returns {Promise<{range_start: number, range_end: number}>}
   */
  _allocate_block_rpc () {
    return new Promise((resolve, reject) => {
      this._grpc_client.AllocateBlock(
        { partition_id: this.partition_id },
        (err, response) => {
          if (err) {
            return reject(err);
          }
          resolve({
            range_start: response.range_start,
            range_end: response.range_end,
          });
        }
      );
    });
  }
}

module.exports = { IdAllocatorGrpcClient };

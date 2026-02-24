"use strict";

const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');

const PROTO_PATH = path.resolve(__dirname, '../../proto/id_allocator.proto');

class MockAllocatorServer {
  constructor (options = {}) {
    this.block_size = options.block_size || 100;
    this.counter = 0;
    this.call_count = 0;
    this.fail_until = options.fail_until || 0;
    this.server = null;
    this.port = null;
  }

  _allocate_block (call, callback) {
    this.call_count++;

    if (this.call_count <= this.fail_until) {
      return callback({
        code: grpc.status.UNAVAILABLE,
        message: 'Mock transient failure',
      });
    }

    const range_start = this.counter;
    this.counter += this.block_size;
    callback(null, { range_start, range_end: this.counter });
  }

  async start () {
    const package_definition = protoLoader.loadSync(PROTO_PATH, {
      keepCase: true,
      longs: Number,
      enums: String,
      defaults: true,
      oneofs: true,
    });
    const proto = grpc.loadPackageDefinition(package_definition);

    this.server = new grpc.Server();
    this.server.addService(proto.id_allocator.IdAllocator.service, {
      AllocateBlock: (call, callback) => this._allocate_block(call, callback),
    });

    return new Promise((resolve, reject) => {
      this.server.bindAsync(
        '127.0.0.1:0',
        grpc.ServerCredentials.createInsecure(),
        (err, port) => {
          if (err) return reject(err);
          this.port = port;
          resolve();
        },
      );
    });
  }

  async stop () {
    if (!this.server) return;
    return new Promise(resolve => {
      this.server.tryShutdown(() => resolve());
    });
  }

  get address () {
    return `127.0.0.1:${this.port}`;
  }
}

module.exports = { MockAllocatorServer };

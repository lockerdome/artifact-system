"use strict";

const { ArtifactGrpcClient } = require('./grpc_client');
const { Snapshot } = require('./snapshot');
const { run_transaction_lifecycle } = require('./transaction');
const { TypeRegistryCache } = require('./type_registry');

/**
 * ArtifactClient is the public entry point for the Artifact Layer.
 * It exposes two factory methods: snapshot() and transaction().
 * All reads and writes go through Snapshot or Transaction objects.
 */
class ArtifactClient {
  /**
   * @param {object} options
   * @param {string} options.service_address - gRPC endpoint (host:port).
   * @param {object} [options.retry] - Retry options for transient gRPC errors.
   * @param {object} [options.channel_credentials] - gRPC channel credentials.
   */
  constructor (options) {
    if (!options || !options.service_address) {
      throw new Error('service_address is required');
    }
    this._grpc_client = new ArtifactGrpcClient(options);
    this._type_registry = new TypeRegistryCache(this._grpc_client);
  }

  async initialize () {
    await this._grpc_client.connect();
  }

  close () {
    this._grpc_client.close();
  }

  /**
   * Create a snapshot of the canonical branch head.
   * @returns {Promise<Snapshot>}
   */
  async snapshot () {
    const response = await this._grpc_client.create_snapshot({});
    return new Snapshot(this._grpc_client, this._type_registry, response.snapshot_id);
  }

  /**
   * Run a callback inside a transaction with auto-commit/rollback.
   *
   * 1. CreateTransaction -> transaction_id
   * 2. Execute callback(txn)
   * 3. On success: CommitTransaction -> { snapshot_id, value }
   * 4. On failure: RollbackTransaction -> re-throw
   * 5. Mark txn as settled (all methods throw TransactionSettledError)
   *
   * @param {function(Transaction): Promise<*>} callback
   * @param {object} [options]
   * @param {string} [options.parent_snapshot_id] - Fork from a specific snapshot instead of the
   *   canonical branch head. To fork from another transaction's branch, use the nested
   *   `txn.transaction()` method instead — top-level transactions only accept snapshot IDs.
   * @returns {Promise<{snapshot_id: string, value: *}>}
   */
  async transaction (callback, options = {}) {
    const create_request = {};
    if (options.parent_snapshot_id) {
      create_request.parent_snapshot_id = options.parent_snapshot_id;
    }

    const create_response = await this._grpc_client.create_transaction(create_request);
    return run_transaction_lifecycle(
      this._grpc_client, this._type_registry, create_response.transaction_id, callback,
    );
  }
}

module.exports = { ArtifactClient };

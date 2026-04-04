"use strict";

const { TransactionSettledError } = require('./errors');
const { Snapshot } = require('./snapshot');

/**
 * Transaction provides reads + writes scoped to a transaction branch.
 * After the transaction callback settles, all methods throw TransactionSettledError.
 *
 * The Transaction does NOT call commit/rollback itself — that is the
 * responsibility of the client.transaction() wrapper.
 */
class Transaction {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {string} transaction_id
   */
  constructor (grpc_client, transaction_id) {
    this._grpc_client = grpc_client;
    this._transaction_id = transaction_id;
    this._settled = false;
  }

  _assert_active () {
    if (this._settled) {
      throw new TransactionSettledError();
    }
  }

  _settle () {
    this._settled = true;
  }

  _read_context () {
    return { transaction_id: this._transaction_id };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Artifact reads
  // ─────────────────────────────────────────────────────────────────────────

  async get (artifact_id) {
    this._assert_active();
    const response = await this._grpc_client.get_artifact({
      artifact_id,
      context: this._read_context(),
    });
    return {
      artifact_id: response.artifact_id,
      type_name: response.type_name,
      version_id: response.version_id,
      payload: response.payload,
    };
  }

  async batch_get (artifact_ids) {
    this._assert_active();
    const response = await this._grpc_client.batch_get_artifacts({
      artifact_ids,
      context: this._read_context(),
    });
    return response.results.map((result) => {
      if (result.artifact) {
        return {
          artifact_id: result.artifact.artifact_id,
          type_name: result.artifact.type_name,
          version_id: result.artifact.version_id,
          payload: result.artifact.payload,
        };
      }
      return null;
    });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Index reads
  // ─────────────────────────────────────────────────────────────────────────

  async fetch_index (key_type, key) {
    this._assert_active();
    const response = await this._grpc_client.fetch_index({
      key_type,
      key,
      context: this._read_context(),
    });
    return {
      index_payload: response.index_payload,
      index_message_name: response.index_message_name,
    };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Type registry reads
  // ─────────────────────────────────────────────────────────────────────────

  async get_type_version (version_id) {
    this._assert_active();
    return await this._grpc_client.get_type_version({ version_id });
  }

  async list_type_versions (type_name) {
    this._assert_active();
    const response = await this._grpc_client.list_type_versions({ type_name });
    return response.version_ids;
  }

  async get_index_schema (key_type) {
    this._assert_active();
    return await this._grpc_client.get_index_schema({ key_type });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Writes
  // ─────────────────────────────────────────────────────────────────────────

  async create (version_id, payload) {
    this._assert_active();
    const response = await this._grpc_client.create_artifact({
      version_id,
      payload,
      transaction_id: this._transaction_id,
    });
    return {
      artifact_id: response.artifact_id,
      snapshot_id: response.snapshot_id,
    };
  }

  async update (artifact_id, version_id, payload) {
    this._assert_active();
    const response = await this._grpc_client.update_artifact({
      artifact_id,
      version_id,
      payload,
      transaction_id: this._transaction_id,
    });
    return { snapshot_id: response.snapshot_id };
  }

  async delete (artifact_id) {
    this._assert_active();
    const response = await this._grpc_client.delete_artifact({
      artifact_id,
      transaction_id: this._transaction_id,
    });
    return { snapshot_id: response.snapshot_id };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Type registry writes
  // ─────────────────────────────────────────────────────────────────────────

  async register_type (type_name, proto_source, options = {}) {
    this._assert_active();
    const request = { type_name, proto_source };
    if (options.deny_create != null) request.deny_create = options.deny_create;
    if (options.deny_update != null) request.deny_update = options.deny_update;
    if (options.deny_delete != null) request.deny_delete = options.deny_delete;
    const response = await this._grpc_client.register_type_version(request);
    return { version_id: response.version_id };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Snapshot from transaction
  // ─────────────────────────────────────────────────────────────────────────

  async snapshot () {
    this._assert_active();
    const response = await this._grpc_client.create_snapshot({
      parent_transaction_id: this._transaction_id,
    });
    return new Snapshot(this._grpc_client, response.snapshot_id);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Nested transactions
  // ─────────────────────────────────────────────────────────────────────────

  async transaction (callback) {
    this._assert_active();

    const response = await this._grpc_client.create_transaction({
      parent_transaction_id: this._transaction_id,
    });
    const sub_txn = new Transaction(this._grpc_client, response.transaction_id);

    try {
      const value = await callback(sub_txn);
      const commit_response = await this._grpc_client.commit_transaction({
        transaction_id: response.transaction_id,
      });
      return { snapshot_id: commit_response.snapshot_id, value };
    } catch (err) {
      try {
        await this._grpc_client.rollback_transaction({
          transaction_id: response.transaction_id,
        });
      } catch (rollback_err) {
        throw new AggregateError([err, rollback_err], err.message);
      }
      throw err;
    } finally {
      sub_txn._settle();
    }
  }
}

module.exports = { Transaction };

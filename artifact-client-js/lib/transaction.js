"use strict";

const { TransactionSettledError } = require('./errors');
const { Readable } = require('./readable');
const { Snapshot } = require('./snapshot');

/**
 * Transaction provides reads + writes scoped to a transaction branch.
 * After the transaction callback settles, all methods throw TransactionSettledError.
 *
 * The Transaction does NOT call commit/rollback itself -- that is the
 * responsibility of the client.transaction() wrapper or run_transaction_lifecycle.
 *
 * Artifact and index payloads are decoded from / encoded to bytes via the
 * shared TypeRegistryCache.  Callers pass and receive JS objects.
 */
class Transaction extends Readable {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {import('./type_registry').TypeRegistryCache} type_registry
   * @param {string} transaction_id
   */
  constructor (grpc_client, type_registry, transaction_id) {
    super(grpc_client, type_registry);
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
  // Read method overrides — add settled guard before delegating to Readable
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * @param {string} artifact_id
   * @returns {Promise<{artifact_id: string, type_name: string, version_id: string, payload: object}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').ArtifactNotFoundError} If the artifact does not exist.
   */
  async get (artifact_id) {
    this._assert_active();
    return super.get(artifact_id);
  }

  /**
   * @param {string[]} artifact_ids
   * @returns {Promise<Array<{artifact_id: string, type_name: string, version_id: string, payload: object} | null>>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').TypeDecodeError} If an artifact has no payload.
   */
  async batch_get (artifact_ids) {
    this._assert_active();
    return super.batch_get(artifact_ids);
  }

  /**
   * @param {string} key_type
   * @param {object} key
   * @returns {Promise<{index_payload: object, index_message_name: string}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').IndexFetchError} If the index entry does not exist.
   */
  async fetch_index (key_type, key) {
    this._assert_active();
    return super.fetch_index(key_type, key);
  }

  /**
   * @param {string} version_id
   * @returns {Promise<object>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   */
  async get_type_version (version_id) {
    this._assert_active();
    return super.get_type_version(version_id);
  }

  /**
   * @param {string} type_name
   * @returns {Promise<string[]>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   */
  async list_type_versions (type_name) {
    this._assert_active();
    return super.list_type_versions(type_name);
  }

  /**
   * @param {string} key_type
   * @returns {Promise<object>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   */
  async get_index_schema (key_type) {
    this._assert_active();
    return super.get_index_schema(key_type);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Writes
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Create a new artifact with the given version and payload.
   * @param {string} version_id - The type version for the new artifact.
   * @param {object} payload - The JS object payload to encode and store.
   * @returns {Promise<{artifact_id: string, snapshot_id: string}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').WriteValidationError} If the payload is invalid.
   */
  async create (version_id, payload) {
    this._assert_active();
    const { payload_bytes } = await this._type_registry.encode_artifact_payload(
      version_id, payload, this._read_context(),
    );
    const response = await this._grpc_client.create_artifact({
      version_id,
      payload: payload_bytes,
      transaction_id: this._transaction_id,
    });
    return {
      artifact_id: response.artifact_id,
      snapshot_id: response.snapshot_id,
    };
  }

  /**
   * Update an existing artifact with a new version and payload.
   * @param {string} artifact_id - The artifact to update.
   * @param {string} version_id - The new type version.
   * @param {object} payload - The new JS object payload to encode and store.
   * @returns {Promise<{snapshot_id: string}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').WriteValidationError} If the payload is invalid.
   */
  async update (artifact_id, version_id, payload) {
    this._assert_active();
    const { payload_bytes } = await this._type_registry.encode_artifact_payload(
      version_id, payload, this._read_context(),
    );
    const response = await this._grpc_client.update_artifact({
      artifact_id,
      version_id,
      payload: payload_bytes,
      transaction_id: this._transaction_id,
    });
    return { snapshot_id: response.snapshot_id };
  }

  /**
   * Delete an artifact by ID.
   * @param {string} artifact_id - The artifact to delete.
   * @returns {Promise<{snapshot_id: string}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   */
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

  /**
   * Register a new type version with the given proto source.
   * @param {string} type_name - The fully qualified type name.
   * @param {string} proto_source - The .proto source text.
   * @param {object} [options] - Optional flags (deny_create, deny_update, deny_delete).
   * @returns {Promise<{version_id: string}>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   * @throws {import('./errors').TypeRegistrationError} If registration fails.
   */
  async register_type (type_name, proto_source, options = {}) {
    this._assert_active();
    const request = {
      type_name,
      proto_source,
      transaction_id: this._transaction_id,
    };
    if (options.deny_create != null) request.deny_create = options.deny_create;
    if (options.deny_update != null) request.deny_update = options.deny_update;
    if (options.deny_delete != null) request.deny_delete = options.deny_delete;
    const response = await this._grpc_client.register_type_version(request);
    return { version_id: response.version_id };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Snapshot from transaction
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Create a snapshot forked from this transaction's current state.
   * @returns {Promise<Snapshot>}
   * @throws {TransactionSettledError} If the transaction has been settled.
   */
  async snapshot () {
    this._assert_active();
    const response = await this._grpc_client.create_snapshot({
      parent_transaction_id: this._transaction_id,
    });
    return new Snapshot(this._grpc_client, this._type_registry, response.snapshot_id);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Nested transactions
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Run a callback inside a nested sub-transaction with auto-commit/rollback.
   * @param {function(Transaction): Promise<*>} callback
   * @returns {Promise<{snapshot_id: string, value: *}>}
   * @throws {TransactionSettledError} If the parent transaction has been settled.
   */
  async transaction (callback) {
    this._assert_active();

    const response = await this._grpc_client.create_transaction({
      parent_transaction_id: this._transaction_id,
    });
    return run_transaction_lifecycle(
      this._grpc_client, this._type_registry, response.transaction_id, callback,
    );
  }
}

/**
 * Shared try/catch/finally logic for running a transaction callback with
 * auto-commit on success and auto-rollback on failure.  Used by both
 * ArtifactClient.transaction() and Transaction.transaction() (nested).
 *
 * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
 * @param {import('./type_registry').TypeRegistryCache} type_registry
 * @param {string} transaction_id - The already-created transaction ID.
 * @param {function(Transaction): Promise<*>} callback
 * @returns {Promise<{snapshot_id: string, value: *}>}
 */
async function run_transaction_lifecycle (grpc_client, type_registry, transaction_id, callback) {
  const txn = new Transaction(grpc_client, type_registry, transaction_id);
  try {
    const value = await callback(txn);
    const commit_response = await grpc_client.commit_transaction({ transaction_id });
    return { snapshot_id: commit_response.snapshot_id, value };
  } catch (err) {
    try {
      await grpc_client.rollback_transaction({ transaction_id });
    } catch (rollback_err) {
      throw new AggregateError([err, rollback_err], err.message);
    }
    throw err;
  } finally {
    txn._settle();
  }
}

module.exports = { Transaction, run_transaction_lifecycle };

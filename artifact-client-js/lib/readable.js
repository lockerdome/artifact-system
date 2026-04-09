"use strict";

const { TypeDecodeError } = require('./errors');

const TO_OBJECT_OPTIONS = { longs: String, enums: String, defaults: true };

/**
 * Readable is the shared base class for Snapshot and Transaction, providing
 * all read-only operations against the artifact layer.  Subclasses must
 * override `_read_context()` to supply the appropriate gRPC read context
 * (snapshot_id or transaction_id).
 */
class Readable {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {import('./type_registry').TypeRegistryCache} type_registry
   */
  constructor (grpc_client, type_registry) {
    this._grpc_client = grpc_client;
    this._type_registry = type_registry;
  }

  /**
   * Return the gRPC ReadContext for this reader.  Must be overridden by
   * subclasses.
   * @returns {{ snapshot_id?: string, transaction_id?: string }}
   */
  _read_context () {
    throw new Error('_read_context() must be implemented by subclass');
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Artifact reads
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Fetch a single artifact by ID with its decoded payload.
   * @param {string} artifact_id - The artifact to retrieve.
   * @returns {Promise<{artifact_id: string, type_name: string, version_id: string, payload: object}>}
   * @throws {import('./errors').ArtifactNotFoundError} If the artifact does not exist.
   */
  async get (artifact_id) {
    const response = await this._grpc_client.get_artifact({
      artifact_id,
      context: this._read_context(),
    });
    const payload = await this._type_registry.decode_artifact_payload(
      response.version_id,
      response.type_name,
      response.payload,
      this._read_context(),
    );
    return {
      artifact_id: response.artifact_id,
      type_name: response.type_name,
      version_id: response.version_id,
      payload,
    };
  }

  /**
   * Fetch multiple artifacts by ID in a single round-trip.
   * Returns `null` for IDs that do not exist.
   * @param {string[]} artifact_ids - The artifacts to retrieve.
   * @returns {Promise<Array<{artifact_id: string, type_name: string, version_id: string, payload: object} | null>>}
   * @throws {TypeDecodeError} If an artifact has no payload.
   */
  async batch_get (artifact_ids) {
    const response = await this._grpc_client.batch_get_artifacts({
      artifact_ids,
      context: this._read_context(),
    });
    return await Promise.all(response.results.map(async (result) => {
      if (!result.artifact) return null;
      if (!result.artifact.payload || result.artifact.payload.length === 0) {
        throw new TypeDecodeError(
          `Artifact ${result.artifact.artifact_id} has no payload`,
        );
      }
      const payload = await this._type_registry.decode_artifact_payload(
        result.artifact.version_id,
        result.artifact.type_name,
        result.artifact.payload,
        this._read_context(),
      );
      return {
        artifact_id: result.artifact.artifact_id,
        type_name: result.artifact.type_name,
        version_id: result.artifact.version_id,
        payload,
      };
    }));
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Index reads
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Look up an index entry by key type and key value.
   * @param {string} key_type - The index key type name.
   * @param {object} key - The key value to look up.
   * @returns {Promise<{index_payload: object, index_message_name: string}>}
   * @throws {import('./errors').IndexFetchError} If the index entry does not exist.
   */
  async fetch_index (key_type, key) {
    const key_bytes = await this._type_registry.encode_index_key(
      key_type, key, this._read_context(),
    );
    const response = await this._grpc_client.fetch_index({
      key_type,
      key: key_bytes,
      context: this._read_context(),
    });
    const index_payload = await this._type_registry.decode_index_payload(
      key_type,
      response.index_message_name,
      response.index_payload,
      this._read_context(),
    );
    return {
      index_payload,
      index_message_name: response.index_message_name,
    };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Type registry reads
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Retrieve metadata for a specific type version.
   * @param {string} version_id - The version to look up.
   * @returns {Promise<object>} Plain JS object with version metadata.
   */
  async get_type_version (version_id) {
    const response = await this._grpc_client.get_type_version({
      version_id,
      read_context: this._read_context(),
    });
    return response.constructor.toObject(response, TO_OBJECT_OPTIONS);
  }

  /**
   * List all version IDs for a given type name.
   * @param {string} type_name - The type to list versions for.
   * @returns {Promise<string[]>}
   */
  async list_type_versions (type_name) {
    const response = await this._grpc_client.list_type_versions({
      type_name,
      read_context: this._read_context(),
    });
    return response.version_ids;
  }

  /**
   * Retrieve the index schema for a given key type.
   * @param {string} key_type - The index key type to look up.
   * @returns {Promise<object>} Plain JS object with index schema.
   */
  async get_index_schema (key_type) {
    const response = await this._grpc_client.get_index_schema({
      key_type,
      read_context: this._read_context(),
    });
    return response.constructor.toObject(response, TO_OBJECT_OPTIONS);
  }
}

module.exports = { Readable, TO_OBJECT_OPTIONS };

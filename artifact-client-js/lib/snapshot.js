"use strict";

/**
 * Snapshot provides a consistent point-in-time read interface.
 * All reads are scoped to this snapshot's ID via a ReadContext.
 * Snapshots are long-lived and can be used freely after creation.
 *
 * Artifact and index payloads are decoded into JS objects using the shared
 * TypeRegistryCache before being returned to the caller.
 */
class Snapshot {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {import('./type_registry').TypeRegistryCache} type_registry
   * @param {string} snapshot_id
   */
  constructor (grpc_client, type_registry, snapshot_id) {
    this._grpc_client = grpc_client;
    this._type_registry = type_registry;
    this._snapshot_id = snapshot_id;
  }

  _read_context () {
    return { snapshot_id: this._snapshot_id };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Artifact reads
  // ─────────────────────────────────────────────────────────────────────────

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

  async batch_get (artifact_ids) {
    const response = await this._grpc_client.batch_get_artifacts({
      artifact_ids,
      context: this._read_context(),
    });
    return await Promise.all(response.results.map(async (result) => {
      if (!result.artifact) return null;
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

  async get_type_version (version_id) {
    return await this._grpc_client.get_type_version({
      version_id,
      read_context: this._read_context(),
    });
  }

  async list_type_versions (type_name) {
    const response = await this._grpc_client.list_type_versions({
      type_name,
      read_context: this._read_context(),
    });
    return response.version_ids;
  }

  async get_index_schema (key_type) {
    return await this._grpc_client.get_index_schema({
      key_type,
      read_context: this._read_context(),
    });
  }
}

module.exports = { Snapshot };

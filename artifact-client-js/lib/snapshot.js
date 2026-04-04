"use strict";

/**
 * Snapshot provides a consistent point-in-time read interface.
 * All reads are scoped to this snapshot's ID via a ReadContext.
 * Snapshots are long-lived and can be used freely after creation.
 */
class Snapshot {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {string} snapshot_id
   */
  constructor (grpc_client, snapshot_id) {
    this._grpc_client = grpc_client;
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
    return {
      artifact_id: response.artifact_id,
      type_name: response.type_name,
      version_id: response.version_id,
      payload: response.payload,
    };
  }

  async batch_get (artifact_ids) {
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
    return await this._grpc_client.get_type_version({ version_id });
  }

  async list_type_versions (type_name) {
    const response = await this._grpc_client.list_type_versions({ type_name });
    return response.version_ids;
  }

  async get_index_schema (key_type) {
    return await this._grpc_client.get_index_schema({ key_type });
  }
}

module.exports = { Snapshot };

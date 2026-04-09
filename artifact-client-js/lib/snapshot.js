"use strict";

const { Readable } = require('./readable');

/**
 * Snapshot provides a consistent point-in-time read interface.
 * All reads are scoped to this snapshot's ID via a ReadContext.
 * Snapshots are long-lived and can be used freely after creation.
 *
 * Artifact and index payloads are decoded into JS objects using the shared
 * TypeRegistryCache before being returned to the caller.
 */
class Snapshot extends Readable {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   * @param {import('./type_registry').TypeRegistryCache} type_registry
   * @param {string} snapshot_id
   */
  constructor (grpc_client, type_registry, snapshot_id) {
    super(grpc_client, type_registry);
    this._snapshot_id = snapshot_id;
  }

  _read_context () {
    return { snapshot_id: this._snapshot_id };
  }
}

module.exports = { Snapshot };

"use strict";

const fs = require('fs');
const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const protobuf = require('protobufjs');
const descriptor = require('protobufjs/ext/descriptor');
const { _error_root } = require('../lib/errors');

const DESCRIPTOR_PATH = path.resolve(__dirname, '../proto/artifact_service.desc');

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture types
//
// The mock server registers a single fixture artifact type and a single
// fixture index schema at construction time, so tests can exercise the full
// encode/decode pipeline without each test having to compile its own protos.
// ─────────────────────────────────────────────────────────────────────────────

const FIXTURE_TYPE_PROTO = `
syntax = "proto3";
package test;
message TestPayload {
  bytes data = 1;
  string label = 2;
}
`;

const FIXTURE_INDEX_PROTO = `
syntax = "proto3";
package test;
message IndexKey_Test {
  string id = 1;
}
message IndexValue_Test {
  uint64 artifact_id = 1;
}
message Index_Test {
  repeated IndexValue_Test entries = 1;
}
`;

const SnakeFileDescriptorSet = _error_root.lookupType('google.protobuf.FileDescriptorSet');
const TypeDefinitionType = _error_root.lookupType('artifact_system.TypeDefinition');

/**
 * Compile a .proto source string into a snake_case JS object form of
 * google.protobuf.FileDescriptorSet, suitable for handing back to the client
 * via @grpc/proto-loader's keepCase decoding.
 *
 * Returns both the JS object form (for the gRPC response) and the
 * protobufjs Root (so the mock can encode/decode payloads with the same
 * Types the client will use after rebuilding from the descriptor).
 */
function compile_proto_to_descriptor_set (proto_source) {
  const root = protobuf.parse(proto_source, { keepCase: true }).root;
  const fds_message = root.toDescriptor('proto3');
  const fds_bytes = descriptor.FileDescriptorSet.encode(fds_message).finish();

  // Re-decode with the snake_case-aware FileDescriptorSet from _error_root so
  // the resulting JS object has snake_case keys, matching what proto-loader
  // expects when encoding the gRPC response.
  const snake_message = SnakeFileDescriptorSet.decode(fds_bytes);
  const descriptor_set_obj = SnakeFileDescriptorSet.toObject(snake_message, {
    longs: String,
    enums: String,
    defaults: false,
    bytes: Buffer,
  });

  return { descriptor_set_obj, root };
}

/**
 * Encode a google.rpc.Status with a single error detail, for returning in
 * grpc-status-details-bin metadata.
 */
function encode_error_detail (grpc_code, message, type_name, detail_obj) {
  const MessageType = _error_root.lookupType(type_name);
  const detail_bytes = MessageType.encode(MessageType.fromObject(detail_obj)).finish();

  const type_url = `type.googleapis.com/${type_name}`;

  const any_writer = protobuf.Writer.create();
  any_writer.uint32(10).string(type_url);
  any_writer.uint32(18).bytes(detail_bytes);
  const any_bytes = any_writer.finish();

  const status_writer = protobuf.Writer.create();
  status_writer.uint32(8).int32(grpc_code);
  status_writer.uint32(18).string(message);
  status_writer.uint32(26).bytes(any_bytes);

  return status_writer.finish();
}

function make_grpc_error_with_detail (code, message, type_name, detail_obj) {
  const metadata = new grpc.Metadata();
  metadata.set('grpc-status-details-bin', Buffer.from(encode_error_detail(code, message, type_name, detail_obj)));
  return { code, message, metadata };
}

/**
 * In-process gRPC server implementing all 4 artifact layer services.
 * Provides configurable in-memory state and failure injection.
 *
 * Auto-registers a fixture artifact type (`test.TestPayload`) and a fixture
 * index schema (`test_index`) at construction so tests can encode/decode
 * payloads without manual proto setup.  See `test_version_id`,
 * `test_type_name`, and `test_index_key_type`.
 */
class MockArtifactServer {
  constructor () {
    this.server = null;
    this.port = null;

    // ── Fixture: artifact type ─────────────────────────────────────────────
    const type_compiled = compile_proto_to_descriptor_set(FIXTURE_TYPE_PROTO);
    this._test_descriptor_set_obj = type_compiled.descriptor_set_obj;
    this._test_root = type_compiled.root;
    this._TestPayloadType = this._test_root.lookupType('test.TestPayload');
    this.test_type_name = 'test.TestPayload';
    this.test_type_id = '1000000';
    this.test_version_id = '1000001';

    // ── Fixture: index schema ──────────────────────────────────────────────
    const index_compiled = compile_proto_to_descriptor_set(FIXTURE_INDEX_PROTO);
    this._test_index_descriptor_set_obj = index_compiled.descriptor_set_obj;
    this._test_index_root = index_compiled.root;
    this._TestIndexKeyType = this._test_index_root.lookupType('test.IndexKey_Test');
    this._TestIndexValueType = this._test_index_root.lookupType('test.IndexValue_Test');
    this._TestIndexType = this._test_index_root.lookupType('test.Index_Test');
    this.test_index_key_type = 'test_index';
    this.test_index_key_message_name = 'test.IndexKey_Test';
    this.test_index_message_name = 'test.Index_Test';

    // In-memory state
    this._next_artifact_id = 1;
    this._next_snapshot_id = 1;
    this._next_transaction_id = 1;
    this._artifacts = new Map(); // artifact_id (string) → { artifact_id, type_name, version_id, payload }
    this._transactions = new Map(); // transaction_id → { parent, committed }
    this._snapshots = new Set(); // snapshot_id set
    this._type_versions = new Map(); // version_id (string) → version info
    this._types = new Map(); // type_name → [version_id, ...]
    this._indexes = new Map(); // "key_type:key_hex" → { index_payload, index_message_name }

    // Call tracking
    this.calls = {};

    // Failure injection: { method_name: { code, message, ... } }
    this._failures = {};

    this._install_fixture_type();
  }

  /**
   * Seed the in-memory state with the fixture type registration so that
   * GetTypeVersion (for test_version_id) and GetArtifact (for test_type_id,
   * which holds the TypeDefinition) both return realistic data.
   */
  _install_fixture_type () {
    this._type_versions.set(this.test_version_id, {
      version_id: this.test_version_id,
      type_id: this.test_type_id,
      descriptor_set: this._test_descriptor_set_obj,
      proto_source: FIXTURE_TYPE_PROTO,
    });
    this._types.set(this.test_type_name, [this.test_version_id]);

    // The TypeDefinition artifact for the test type lives at test_type_id.
    // Its payload is a TypeDefinition message; the client decodes it to
    // resolve version_id → type_name.
    const type_def_payload = TypeDefinitionType.encode(
      TypeDefinitionType.fromObject({ type_name: this.test_type_name }),
    ).finish();

    this._artifacts.set(this.test_type_id, {
      artifact_id: this.test_type_id,
      type_name: 'artifact_system.TypeDefinition',
      version_id: this.test_version_id,
      payload: Buffer.from(type_def_payload),
    });
  }

  _track_call (method) {
    this.calls[method] = (this.calls[method] || 0) + 1;
  }

  set_failure (method, error) {
    if (error) {
      this._failures[method] = error;
    } else {
      delete this._failures[method];
    }
  }

  _check_failure (method, callback) {
    const failure = this._failures[method];
    if (failure) {
      return callback(failure);
    }
    return null;
  }

  _new_snapshot_id () {
    const id = `snap-${this._next_snapshot_id++}`;
    this._snapshots.add(id);
    return id;
  }

  _new_transaction_id () {
    return `txn-${this._next_transaction_id++}`;
  }

  _new_artifact_id () {
    return String(this._next_artifact_id++);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // SnapshotTransactionService
  // ─────────────────────────────────────────────────────────────────────────

  _create_snapshot (call, callback) {
    this._track_call('CreateSnapshot');
    if (this._check_failure('CreateSnapshot', callback)) return;

    const snapshot_id = this._new_snapshot_id();
    callback(null, { snapshot_id });
  }

  _create_transaction (call, callback) {
    this._track_call('CreateTransaction');
    if (this._check_failure('CreateTransaction', callback)) return;

    const transaction_id = this._new_transaction_id();
    this._transactions.set(transaction_id, {
      parent: call.request.parent_snapshot_id || call.request.parent_transaction_id || null,
      committed: false,
    });
    callback(null, { transaction_id });
  }

  _commit_transaction (call, callback) {
    this._track_call('CommitTransaction');
    if (this._check_failure('CommitTransaction', callback)) return;

    const { transaction_id } = call.request;
    const txn = this._transactions.get(transaction_id);
    if (!txn) {
      return callback(make_grpc_error_with_detail(
        grpc.status.NOT_FOUND,
        'Transaction not found',
        'artifact_system.SnapshotTransactionError',
        { category: 'TRANSACTION_NOT_FOUND', description: 'Transaction not found', id: transaction_id },
      ));
    }
    txn.committed = true;
    const snapshot_id = this._new_snapshot_id();
    callback(null, { snapshot_id });
  }

  _rollback_transaction (call, callback) {
    this._track_call('RollbackTransaction');
    if (this._check_failure('RollbackTransaction', callback)) return;

    const { transaction_id } = call.request;
    this._transactions.delete(transaction_id);
    callback(null, {});
  }

  // ─────────────────────────────────────────────────────────────────────────
  // ArtifactService
  // ─────────────────────────────────────────────────────────────────────────

  _create_artifact (call, callback) {
    this._track_call('CreateArtifact');
    if (this._check_failure('CreateArtifact', callback)) return;

    const { version_id, payload } = call.request;
    const artifact_id = this._new_artifact_id();
    this._artifacts.set(artifact_id, {
      artifact_id,
      type_name: this.test_type_name,
      version_id,
      payload,
    });
    const snapshot_id = this._new_snapshot_id();
    callback(null, { artifact_id, snapshot_id });
  }

  _get_artifact (call, callback) {
    this._track_call('GetArtifact');
    if (this._check_failure('GetArtifact', callback)) return;

    const { artifact_id } = call.request;
    const artifact = this._artifacts.get(artifact_id);
    if (!artifact) {
      return callback(make_grpc_error_with_detail(
        grpc.status.NOT_FOUND,
        'Artifact not found',
        'artifact_system.ArtifactNotFoundError',
        { artifact_id: parseInt(artifact_id) || 0, tombstoned: false },
      ));
    }
    callback(null, artifact);
  }

  _batch_get_artifacts (call, callback) {
    this._track_call('BatchGetArtifacts');
    if (this._check_failure('BatchGetArtifacts', callback)) return;

    const results = call.request.artifact_ids.map((id) => {
      const artifact = this._artifacts.get(id);
      if (artifact) {
        return { artifact };
      }
      return { not_found: { artifact_id: id, tombstoned: false } };
    });
    callback(null, { results });
  }

  _update_artifact (call, callback) {
    this._track_call('UpdateArtifact');
    if (this._check_failure('UpdateArtifact', callback)) return;

    const { artifact_id, version_id, payload } = call.request;
    const artifact = this._artifacts.get(artifact_id);
    if (!artifact) {
      return callback(make_grpc_error_with_detail(
        grpc.status.NOT_FOUND,
        'Artifact not found',
        'artifact_system.ArtifactNotFoundError',
        { artifact_id: parseInt(artifact_id) || 0, tombstoned: false },
      ));
    }
    artifact.version_id = version_id;
    artifact.payload = payload;
    const snapshot_id = this._new_snapshot_id();
    callback(null, { snapshot_id });
  }

  _delete_artifact (call, callback) {
    this._track_call('DeleteArtifact');
    if (this._check_failure('DeleteArtifact', callback)) return;

    const { artifact_id } = call.request;
    this._artifacts.delete(artifact_id);
    const snapshot_id = this._new_snapshot_id();
    callback(null, { snapshot_id });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // IndexService
  // ─────────────────────────────────────────────────────────────────────────

  _fetch_index (call, callback) {
    this._track_call('FetchIndex');
    if (this._check_failure('FetchIndex', callback)) return;

    const { key_type, key } = call.request;
    const key_hex = Buffer.isBuffer(key) ? key.toString('hex') : Buffer.from(key).toString('hex');
    const index_key = `${key_type}:${key_hex}`;
    const entry = this._indexes.get(index_key);
    if (!entry) {
      return callback(make_grpc_error_with_detail(
        grpc.status.NOT_FOUND,
        'Index not found',
        'artifact_system.FetchIndexError',
        { category: 'INDEX_NOT_FOUND', description: 'Index not found', key_type },
      ));
    }
    callback(null, entry);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // TypeRegistryService
  // ─────────────────────────────────────────────────────────────────────────

  _register_type_version (call, callback) {
    this._track_call('RegisterTypeVersion');
    if (this._check_failure('RegisterTypeVersion', callback)) return;

    const { type_name, proto_source } = call.request;
    const version_id = this._new_artifact_id();

    // Reuse the fixture descriptor_set so any subsequent decode against this
    // version_id finds *some* valid type definition; tests that exercise the
    // register-type path don't depend on the user's proto_source actually
    // being compiled.
    this._type_versions.set(version_id, {
      version_id,
      type_id: this.test_type_id,
      proto_source,
      descriptor_set: this._test_descriptor_set_obj,
    });

    if (!this._types.has(type_name)) {
      this._types.set(type_name, []);
    }
    this._types.get(type_name).push(version_id);

    callback(null, { version_id });
  }

  _get_type_version (call, callback) {
    this._track_call('GetTypeVersion');
    if (this._check_failure('GetTypeVersion', callback)) return;

    const { version_id } = call.request;
    const version = this._type_versions.get(version_id);
    if (!version) {
      return callback({
        code: grpc.status.NOT_FOUND,
        message: 'Type version not found',
      });
    }
    callback(null, version);
  }

  _list_type_versions (call, callback) {
    this._track_call('ListTypeVersions');
    if (this._check_failure('ListTypeVersions', callback)) return;

    const { type_name } = call.request;
    const version_ids = this._types.get(type_name) || [];
    callback(null, { version_ids });
  }

  _get_index_schema (call, callback) {
    this._track_call('GetIndexSchema');
    if (this._check_failure('GetIndexSchema', callback)) return;

    callback(null, {
      index_definition_id: '1',
      key_type: call.request.key_type,
      key_fields: ['id'],
      order_fields: [],
      unique: false,
      index_descriptor_set: this._test_index_descriptor_set_obj,
      key_message_name: this.test_index_key_message_name,
      value_message_name: 'test.IndexValue_Test',
      index_message_name: this.test_index_message_name,
    });
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Server lifecycle
  // ─────────────────────────────────────────────────────────────────────────

  async start () {
    const descriptor_bytes = fs.readFileSync(DESCRIPTOR_PATH);
    const package_definition = protoLoader.loadFileDescriptorSetFromBuffer(descriptor_bytes, {
      keepCase: true,
      longs: String,
      enums: String,
      defaults: true,
      oneofs: true,
    });
    const proto = grpc.loadPackageDefinition(package_definition);
    const pkg = proto.artifact_system;

    this.server = new grpc.Server();

    this.server.addService(pkg.SnapshotTransactionService.service, {
      CreateSnapshot: (call, cb) => this._create_snapshot(call, cb),
      CreateTransaction: (call, cb) => this._create_transaction(call, cb),
      CommitTransaction: (call, cb) => this._commit_transaction(call, cb),
      RollbackTransaction: (call, cb) => this._rollback_transaction(call, cb),
    });

    this.server.addService(pkg.ArtifactService.service, {
      CreateArtifact: (call, cb) => this._create_artifact(call, cb),
      GetArtifact: (call, cb) => this._get_artifact(call, cb),
      BatchGetArtifacts: (call, cb) => this._batch_get_artifacts(call, cb),
      UpdateArtifact: (call, cb) => this._update_artifact(call, cb),
      DeleteArtifact: (call, cb) => this._delete_artifact(call, cb),
    });

    this.server.addService(pkg.IndexService.service, {
      FetchIndex: (call, cb) => this._fetch_index(call, cb),
    });

    this.server.addService(pkg.TypeRegistryService.service, {
      RegisterTypeVersion: (call, cb) => this._register_type_version(call, cb),
      GetTypeVersion: (call, cb) => this._get_type_version(call, cb),
      ListTypeVersions: (call, cb) => this._list_type_versions(call, cb),
      GetIndexSchema: (call, cb) => this._get_index_schema(call, cb),
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

  /**
   * Encode a JS payload object using the fixture test type and return the
   * resulting bytes.  Useful when tests need to compare against the wire
   * form directly.
   */
  encode_test_payload (payload_obj) {
    return Buffer.from(
      this._TestPayloadType.encode(this._TestPayloadType.fromObject(payload_obj)).finish(),
    );
  }

  /**
   * Encode a JS index key object using the fixture index schema.
   */
  encode_test_index_key (key_obj) {
    return Buffer.from(
      this._TestIndexKeyType.encode(this._TestIndexKeyType.fromObject(key_obj)).finish(),
    );
  }

  /**
   * Encode a JS index payload object using the fixture index schema.
   */
  encode_test_index_payload (index_obj) {
    return Buffer.from(
      this._TestIndexType.encode(this._TestIndexType.fromObject(index_obj)).finish(),
    );
  }

  /**
   * Seed an artifact directly into the in-memory store.
   *
   * Accepts a payload as either:
   *   - a plain JS object: encoded via the fixture test type
   *   - a Buffer / Uint8Array: stored as-is (caller is responsible for
   *     making sure the bytes are decodable against `version_id`)
   *
   * The artifact is registered as the fixture test type unless `type_name`
   * and `version_id` are explicitly provided.
   */
  seed_artifact (artifact_id, payload, options = {}) {
    const type_name = options.type_name || this.test_type_name;
    const version_id = options.version_id || this.test_version_id;
    let payload_bytes;
    if (Buffer.isBuffer(payload) || payload instanceof Uint8Array) {
      payload_bytes = Buffer.from(payload);
    } else {
      payload_bytes = this.encode_test_payload(payload);
    }
    this._artifacts.set(artifact_id, {
      artifact_id,
      type_name,
      version_id,
      payload: payload_bytes,
    });
  }

  /**
   * Seed an index entry directly into the in-memory store.
   *
   * Accepts key/value as JS objects (encoded via fixture index schema) or
   * raw bytes.
   */
  seed_index (key, index_payload, options = {}) {
    const key_type = options.key_type || this.test_index_key_type;
    const index_message_name = options.index_message_name || this.test_index_message_name;

    let key_bytes;
    if (Buffer.isBuffer(key) || key instanceof Uint8Array) {
      key_bytes = Buffer.from(key);
    } else {
      key_bytes = this.encode_test_index_key(key);
    }

    let index_payload_bytes;
    if (Buffer.isBuffer(index_payload) || index_payload instanceof Uint8Array) {
      index_payload_bytes = Buffer.from(index_payload);
    } else {
      index_payload_bytes = this.encode_test_index_payload(index_payload);
    }

    const key_hex = key_bytes.toString('hex');
    this._indexes.set(`${key_type}:${key_hex}`, {
      index_payload: index_payload_bytes,
      index_message_name,
    });
  }

  /**
   * Reset all in-memory state and call tracking.  Re-installs the fixture
   * type so subsequent reads can still resolve descriptor_set / type_name.
   */
  reset () {
    this._next_artifact_id = 1;
    this._next_snapshot_id = 1;
    this._next_transaction_id = 1;
    this._artifacts.clear();
    this._transactions.clear();
    this._snapshots.clear();
    this._type_versions.clear();
    this._types.clear();
    this._indexes.clear();
    this.calls = {};
    this._failures = {};
    this._install_fixture_type();
  }
}

module.exports = { MockArtifactServer, make_grpc_error_with_detail, encode_error_detail };

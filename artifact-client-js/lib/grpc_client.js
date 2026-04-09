"use strict";

const fs = require('fs');
const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const { retry_with_backoff, is_retryable, DEFAULT_RETRY_OPTIONS } = require('./retry');
const { parse_grpc_error, _error_root } = require('./errors');

const DEFAULT_CALL_TIMEOUT_MS = 30000;

const DESCRIPTOR_PATH = path.resolve(__dirname, '../proto/artifact_service.desc');

// ─────────────────────────────────────────────────────────────────────────────
// protobufjs-based serializers for RPCs that return descriptor sets.
//
// The proto-loader stubs decode responses with `defaults: true`, which
// populates absent optional fields with their default values.  For
// google.protobuf.FileDescriptorSet sub-messages this is destructive:
// Google's descriptor.proto (which defines FieldDescriptorProto) is
// written in proto2 and declares `optional int32 oneof_index`.  Under
// `defaults: true`, that field gets set to 0 on every field descriptor —
// even non-oneof fields — and the client cannot distinguish between
// "genuinely in oneof 0" and "defaulted to 0" afterwards.
//
// Instead of trying to scrub these spurious defaults, we bypass proto-loader
// entirely for GetTypeVersion and GetIndexSchema.  The request/response
// types are loaded from _error_root (which has snake_case field names,
// matching the rest of the client), and grpc-js's makeUnaryRequest is used
// with custom serialize/deserialize functions.  The descriptor_set field
// arrives as a protobufjs Message — no defaults, no conversion — and is
// passed directly to protobuf.Root.fromDescriptor().
// ─────────────────────────────────────────────────────────────────────────────

const _GetTypeVersionRequest = _error_root.lookupType('artifact_system.GetTypeVersionRequest');
const _GetTypeVersionResponse = _error_root.lookupType('artifact_system.GetTypeVersionResponse');
const _GetIndexSchemaRequest = _error_root.lookupType('artifact_system.GetIndexSchemaRequest');
const _GetIndexSchemaResponse = _error_root.lookupType('artifact_system.GetIndexSchemaResponse');

function _protobuf_serialize (MessageType) {
  return (obj) => Buffer.from(MessageType.encode(MessageType.fromObject(obj)).finish());
}

function _protobuf_deserialize (MessageType) {
  return (bytes) => MessageType.decode(bytes);
}

/**
 * Low-level gRPC transport for the Artifact Layer's four services.
 * Loads protos, creates stubs, promisifies RPCs, and handles error parsing.
 */
class ArtifactGrpcClient {
  /**
   * @param {object} options
   * @param {string} options.service_address - gRPC endpoint (host:port).
   * @param {object} [options.retry] - Retry options for transient errors.
   * @param {object} [options.channel_credentials] - gRPC channel credentials.
   * @param {number} [options.call_timeout_ms] - Per-call gRPC deadline in milliseconds (default: 30000).
   */
  constructor (options) {
    if (!options || !options.service_address) {
      throw new Error('service_address is required');
    }

    this.service_address = options.service_address;
    this.retry_options = { ...DEFAULT_RETRY_OPTIONS, ...options.retry };
    this.channel_credentials = options.channel_credentials ?? grpc.credentials.createInsecure();
    this.call_timeout_ms = options.call_timeout_ms ?? DEFAULT_CALL_TIMEOUT_MS;

    this._snapshot_transaction_client = null;
    this._artifact_client = null;
    this._index_client = null;
    this._type_registry_client = null;
    this._connected = false;
  }

  connect () {
    if (this._connected) return;

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

    // Create the first client normally — it owns the underlying gRPC channel.
    // All remaining clients reuse that channel via channelOverride so that the
    // four service stubs share a single HTTP/2 connection instead of each
    // opening its own TCP connection.
    this._snapshot_transaction_client = new pkg.SnapshotTransactionService(
      this.service_address, this.channel_credentials,
    );

    const shared_channel = this._snapshot_transaction_client.getChannel();

    this._artifact_client = new pkg.ArtifactService(
      this.service_address, this.channel_credentials,
      { channelOverride: shared_channel },
    );
    this._index_client = new pkg.IndexService(
      this.service_address, this.channel_credentials,
      { channelOverride: shared_channel },
    );
    this._type_registry_client = new pkg.TypeRegistryService(
      this.service_address, this.channel_credentials,
      { channelOverride: shared_channel },
    );

    this._connected = true;
  }

  close () {
    if (this._snapshot_transaction_client) this._snapshot_transaction_client.close();
    if (this._artifact_client) this._artifact_client.close();
    if (this._index_client) this._index_client.close();
    if (this._type_registry_client) this._type_registry_client.close();
    this._snapshot_transaction_client = null;
    this._artifact_client = null;
    this._index_client = null;
    this._type_registry_client = null;
    this._connected = false;
  }

  // ─────────────────────────────────────────────────────────────────────────
  // SnapshotTransactionService
  // ─────────────────────────────────────────────────────────────────────────

  create_snapshot (request) {
    return this._call(this._snapshot_transaction_client, 'CreateSnapshot', request);
  }

  create_transaction (request) {
    return this._call(this._snapshot_transaction_client, 'CreateTransaction', request);
  }

  commit_transaction (request) {
    return this._invoke(this._snapshot_transaction_client, 'CommitTransaction', request);
  }

  rollback_transaction (request) {
    return this._invoke(this._snapshot_transaction_client, 'RollbackTransaction', request);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // ArtifactService
  // ─────────────────────────────────────────────────────────────────────────

  create_artifact (request) {
    return this._invoke(this._artifact_client, 'CreateArtifact', request);
  }

  get_artifact (request) {
    return this._call(this._artifact_client, 'GetArtifact', request);
  }

  batch_get_artifacts (request) {
    return this._call(this._artifact_client, 'BatchGetArtifacts', request);
  }

  update_artifact (request) {
    return this._invoke(this._artifact_client, 'UpdateArtifact', request);
  }

  delete_artifact (request) {
    return this._invoke(this._artifact_client, 'DeleteArtifact', request);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // IndexService
  // ─────────────────────────────────────────────────────────────────────────

  fetch_index (request) {
    return this._call(this._index_client, 'FetchIndex', request);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // TypeRegistryService
  // ─────────────────────────────────────────────────────────────────────────

  register_type_version (request) {
    return this._invoke(this._type_registry_client, 'RegisterTypeVersion', request);
  }

  get_type_version (request) {
    return this._call_raw(
      this._type_registry_client,
      '/artifact_system.TypeRegistryService/GetTypeVersion',
      _GetTypeVersionRequest,
      _GetTypeVersionResponse,
      request,
    );
  }

  list_type_versions (request) {
    return this._call(this._type_registry_client, 'ListTypeVersions', request);
  }

  get_index_schema (request) {
    return this._call_raw(
      this._type_registry_client,
      '/artifact_system.TypeRegistryService/GetIndexSchema',
      _GetIndexSchemaRequest,
      _GetIndexSchemaResponse,
      request,
    );
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Internal
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Build a gRPC callback that resolves/rejects a promise, parsing errors
   * into typed ArtifactErrors.
   */
  _create_invoke_callback (resolve, reject) {
    return (err, response) => {
      if (err) {
        try {
          parse_grpc_error(err);
        } catch (parsed) {
          return reject(parsed);
        }
        return reject(err); // defensive: parse_grpc_error should always throw
      }
      resolve(response);
    };
  }

  /**
   * Single-attempt unary RPC using proto-loader stubs.
   */
  _invoke (client, method, request) {
    if (!this._connected) {
      throw new Error('Client is not connected. Call connect() first.');
    }
    const deadline = new Date(Date.now() + this.call_timeout_ms);
    return new Promise((resolve, reject) => {
      client[method](request, { deadline }, this._create_invoke_callback(resolve, reject));
    });
  }

  /**
   * Single-attempt unary RPC using protobufjs serialization (bypassing
   * proto-loader).  Used for RPCs whose responses contain descriptor set
   * sub-messages — see the module-level comment for rationale.
   */
  _invoke_raw (client, method_path, RequestType, ResponseType, request) {
    if (!this._connected) {
      throw new Error('Client is not connected. Call connect() first.');
    }
    const deadline = new Date(Date.now() + this.call_timeout_ms);
    return new Promise((resolve, reject) => {
      client.makeUnaryRequest(
        method_path,
        _protobuf_serialize(RequestType),
        _protobuf_deserialize(ResponseType),
        request,
        { deadline },
        this._create_invoke_callback(resolve, reject),
      );
    });
  }

  /**
   * Unary RPC with retry on transient errors.  Used for idempotent reads.
   * Errors are always parsed into typed ArtifactErrors before being
   * checked for retryability, so callers never see raw gRPC errors.
   */
  _call (client, method, request) {
    return retry_with_backoff(
      () => this._invoke(client, method, request),
      this.retry_options,
    );
  }

  /**
   * Unary RPC with retry, using protobufjs serialization.
   */
  _call_raw (client, method_path, RequestType, ResponseType, request) {
    return retry_with_backoff(
      () => this._invoke_raw(client, method_path, RequestType, ResponseType, request),
      this.retry_options,
    );
  }

}

module.exports = { ArtifactGrpcClient };

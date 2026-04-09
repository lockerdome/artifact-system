"use strict";

const fs = require('fs');
const path = require('path');
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const { retry_with_backoff, is_retryable } = require('./retry');
const { parse_grpc_error, _error_root } = require('./errors');

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

const DEFAULT_RETRY_OPTIONS = {
  max_retries: 5,
  base_delay_ms: 100,
  max_delay_ms: 10000,
};

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
   */
  constructor (options) {
    if (!options || !options.service_address) {
      throw new Error('service_address is required');
    }

    this.service_address = options.service_address;
    this.retry_options = { ...DEFAULT_RETRY_OPTIONS, ...options.retry };
    this.channel_credentials = options.channel_credentials ?? grpc.credentials.createInsecure();

    this._snapshot_transaction_client = null;
    this._artifact_client = null;
    this._index_client = null;
    this._type_registry_client = null;
    this._connected = false;
  }

  async connect () {
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

    this._snapshot_transaction_client = new pkg.SnapshotTransactionService(
      this.service_address, this.channel_credentials,
    );
    this._artifact_client = new pkg.ArtifactService(
      this.service_address, this.channel_credentials,
    );
    this._index_client = new pkg.IndexService(
      this.service_address, this.channel_credentials,
    );
    this._type_registry_client = new pkg.TypeRegistryService(
      this.service_address, this.channel_credentials,
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
    return this._call(this._snapshot_transaction_client, 'CommitTransaction', request);
  }

  rollback_transaction (request) {
    return this._call(this._snapshot_transaction_client, 'RollbackTransaction', request);
  }

  // ─────────────────────────────────────────────────────────────────────────
  // ArtifactService
  // ─────────────────────────────────────────────────────────────────────────

  create_artifact (request) {
    return this._call(this._artifact_client, 'CreateArtifact', request);
  }

  get_artifact (request) {
    return this._call(this._artifact_client, 'GetArtifact', request);
  }

  batch_get_artifacts (request) {
    return this._call(this._artifact_client, 'BatchGetArtifacts', request);
  }

  update_artifact (request) {
    return this._call(this._artifact_client, 'UpdateArtifact', request);
  }

  delete_artifact (request) {
    return this._call(this._artifact_client, 'DeleteArtifact', request);
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
    return this._call(this._type_registry_client, 'RegisterTypeVersion', request);
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
   * Make a unary RPC call using protobufjs serialization (bypassing
   * proto-loader).  Used for RPCs whose responses contain descriptor set
   * sub-messages — see the module-level comment for rationale.
   */
  _call_raw (client, method_path, RequestType, ResponseType, request) {
    if (!this._connected) {
      throw new Error('Client is not connected. Call connect() first.');
    }

    return retry_with_backoff(() => {
      return new Promise((resolve, reject) => {
        client.makeUnaryRequest(
          method_path,
          _protobuf_serialize(RequestType),
          _protobuf_deserialize(ResponseType),
          request,
          (err, response) => {
            if (err) {
              if (is_retryable(err)) {
                return reject(err);
              }
              try {
                parse_grpc_error(err);
              } catch (parsed) {
                return reject(parsed);
              }
            }
            resolve(response);
          },
        );
      });
    }, this.retry_options);
  }

  /**
   * Make a unary RPC call with retry on transient errors and error parsing.
   */
  _call (client, method, request) {
    if (!this._connected) {
      throw new Error('Client is not connected. Call connect() first.');
    }

    return retry_with_backoff(() => {
      return new Promise((resolve, reject) => {
        client[method](request, (err, response) => {
          if (err) {
            if (is_retryable(err)) {
              return reject(err);
            }
            try {
              parse_grpc_error(err);
            } catch (parsed) {
              return reject(parsed);
            }
          }
          resolve(response);
        });
      });
    }, this.retry_options);
  }
}

module.exports = { ArtifactGrpcClient };

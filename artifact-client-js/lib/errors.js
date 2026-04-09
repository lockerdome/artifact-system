"use strict";

const fs = require('fs');
const path = require('path');
const protobuf = require('protobufjs');

// ─────────────────────────────────────────────────────────────────────────────
// Error classes
// ─────────────────────────────────────────────────────────────────────────────

class ArtifactError extends Error {
  constructor (message, code, detail, grpc_error) {
    super(message);
    this.name = this.constructor.name;
    this.code = code;
    this.detail = detail ?? null;
    this.grpc_error = grpc_error ?? null;
  }
}

class ArtifactNotFoundError extends ArtifactError {}
class WriteValidationError extends ArtifactError {}
class ConflictError extends ArtifactError {}
class TransactionError extends ArtifactError {}
class IndexFetchError extends ArtifactError {}
class TypeRegistrationError extends ArtifactError {}

class TransactionSettledError extends Error {
  constructor (message) {
    super(message || 'Transaction has already been settled');
    this.name = 'TransactionSettledError';
  }
}

/**
 * Thrown when the client cannot resolve or apply a protobuf type while
 * decoding/encoding an artifact payload or index payload (e.g., the named
 * message is missing from the descriptor set returned by the server).
 */
class TypeDecodeError extends Error {
  constructor (message) {
    super(message);
    this.name = 'TypeDecodeError';
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Error detail protobuf types — loaded from pre-compiled FileDescriptorSet
// ─────────────────────────────────────────────────────────────────────────────

const DESCRIPTOR_PATH = path.resolve(__dirname, '../proto/artifact_service.desc');
const descriptor = require('protobufjs/ext/descriptor');

const _descriptor_bytes = fs.readFileSync(DESCRIPTOR_PATH);
const _decoded_descriptor = descriptor.FileDescriptorSet.decode(_descriptor_bytes);
const _error_root = protobuf.Root.fromDescriptor(_decoded_descriptor);

// Maps type_url suffix → { MessageType, ErrorClass }
const DETAIL_TYPE_MAP = {
  'artifact_system.ArtifactNotFoundError': {
    MessageType: _error_root.lookupType('artifact_system.ArtifactNotFoundError'),
    ErrorClass: ArtifactNotFoundError,
  },
  'artifact_system.ArtifactWriteError': {
    MessageType: _error_root.lookupType('artifact_system.ArtifactWriteError'),
    ErrorClass: WriteValidationError,
  },
  'artifact_system.CommitConflict': {
    MessageType: _error_root.lookupType('artifact_system.CommitConflict'),
    ErrorClass: ConflictError,
  },
  'artifact_system.SnapshotTransactionError': {
    MessageType: _error_root.lookupType('artifact_system.SnapshotTransactionError'),
    ErrorClass: TransactionError,
  },
  'artifact_system.FetchIndexError': {
    MessageType: _error_root.lookupType('artifact_system.FetchIndexError'),
    ErrorClass: IndexFetchError,
  },
  'artifact_system.RegisterTypeVersionError': {
    MessageType: _error_root.lookupType('artifact_system.RegisterTypeVersionError'),
    ErrorClass: TypeRegistrationError,
  },
};

// ─────────────────────────────────────────────────────────────────────────────
// gRPC error detail parsing
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Extract the fully qualified type name from a google.protobuf.Any type_url.
 * type_url is typically "type.googleapis.com/package.MessageName".
 */
function extract_type_name (type_url) {
  const slash_index = type_url.lastIndexOf('/');
  return slash_index >= 0 ? type_url.substring(slash_index + 1) : type_url;
}

/**
 * Decode a google.rpc.Status message from raw bytes.
 * Manual decode since we don't have google/rpc/status.proto available.
 *
 * Wire format:
 *   field 1 (varint): code
 *   field 2 (length-delimited): message
 *   field 3 (length-delimited, repeated): details (google.protobuf.Any)
 *
 * Each Any:
 *   field 1 (length-delimited): type_url
 *   field 2 (length-delimited): value
 */
function decode_rpc_status (buffer) {
  const reader = protobuf.Reader.create(buffer);
  const status = { code: 0, message: '', details: [] };

  while (reader.pos < reader.len) {
    const tag = reader.uint32();
    const field_number = tag >>> 3;
    const wire_type = tag & 7;

    switch (field_number) {
      case 1:
        status.code = reader.int32();
        break;
      case 2:
        status.message = reader.string();
        break;
      case 3: {
        const any_bytes = reader.bytes();
        const any_reader = protobuf.Reader.create(any_bytes);
        const any = { type_url: '', value: Buffer.alloc(0) };
        while (any_reader.pos < any_reader.len) {
          const any_tag = any_reader.uint32();
          const any_field = any_tag >>> 3;
          switch (any_field) {
            case 1:
              any.type_url = any_reader.string();
              break;
            case 2:
              any.value = any_reader.bytes();
              break;
            default:
              any_reader.skipType(any_tag & 7);
              break;
          }
        }
        status.details.push(any);
        break;
      }
      default:
        reader.skipType(wire_type);
        break;
    }
  }

  return status;
}

/**
 * Parse a gRPC error into a typed ArtifactError subclass.
 * Decodes the grpc-status-details-bin metadata trailer to extract error details.
 *
 * @param {Error} grpc_error - The error from a gRPC call.
 * @throws {ArtifactError} A typed error subclass if a known detail is found.
 * @throws {ArtifactError} A generic ArtifactError if no known detail is found.
 */
function parse_grpc_error (grpc_error) {
  const metadata = grpc_error.metadata;
  const detail_bin = metadata ? metadata.get('grpc-status-details-bin') : null;

  if (detail_bin && detail_bin.length > 0) {
    const status_bytes = detail_bin[0];
    if (Buffer.isBuffer(status_bytes) || status_bytes instanceof Uint8Array) {
      const status = decode_rpc_status(status_bytes);

      for (const any of status.details) {
        const type_name = extract_type_name(any.type_url);
        const mapping = DETAIL_TYPE_MAP[type_name];

        if (mapping) {
          let detail = null;
          try {
            const decoded = mapping.MessageType.decode(any.value);
            detail = mapping.MessageType.toObject(decoded, {
              longs: String,
              enums: String,
              defaults: true,
            });
          } catch (_) {
            // If decoding fails, leave detail as null
          }

          throw new mapping.ErrorClass(
            grpc_error.message || status.message,
            grpc_error.code,
            detail,
            grpc_error,
          );
        }
      }
    }
  }

  // No recognized detail — throw generic ArtifactError
  throw new ArtifactError(
    grpc_error.message || 'Unknown gRPC error',
    grpc_error.code,
    null,
    grpc_error,
  );
}

module.exports = {
  ArtifactError,
  ArtifactNotFoundError,
  WriteValidationError,
  ConflictError,
  TransactionError,
  TransactionSettledError,
  TypeDecodeError,
  IndexFetchError,
  TypeRegistrationError,
  parse_grpc_error,
  // Exported for testing
  decode_rpc_status,
  extract_type_name,
  _error_root,
};

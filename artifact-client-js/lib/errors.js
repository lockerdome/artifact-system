"use strict";

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

// ─────────────────────────────────────────────────────────────────────────────
// Error detail protobuf types (defined programmatically to avoid loading
// artifact_options.proto which uses syntax protobufjs cannot parse)
// ─────────────────────────────────────────────────────────────────────────────

const _error_proto_json = {
  nested: {
    artifact_system: {
      nested: {
        ArtifactNotFoundError: {
          fields: {
            artifact_id: { type: 'uint64', id: 1 },
            tombstoned: { type: 'bool', id: 2 },
          },
        },
        ArtifactWriteError: {
          fields: {
            violations: { rule: 'repeated', type: 'ArtifactWriteViolation', id: 1 },
          },
        },
        ArtifactWriteViolation: {
          fields: {
            category: { type: 'string', id: 1 },
            description: { type: 'string', id: 2 },
            subject: { type: 'string', id: 3 },
          },
        },
        CommitConflict: {
          fields: {
            conflict_type: { type: 'string', id: 1 },
            retryable: { type: 'bool', id: 2 },
            attempts: { type: 'uint32', id: 3 },
            payload_detail: { type: 'PayloadConflictDetail', id: 4 },
            index_detail: { type: 'IndexConflictDetail', id: 5 },
            referential_integrity_detail: { type: 'ReferentialIntegrityConflictDetail', id: 6 },
            base_commit_id: { type: 'string', id: 7 },
            ours_commit_id: { type: 'string', id: 8 },
            theirs_commit_id: { type: 'string', id: 9 },
          },
          oneofs: {
            detail: { oneof: ['payload_detail', 'index_detail', 'referential_integrity_detail'] },
          },
        },
        PayloadConflictDetail: {
          fields: {
            artifact_id: { type: 'uint64', id: 1 },
          },
        },
        IndexConflictDetail: {
          fields: {
            key_type: { type: 'string', id: 1 },
            encoded_key: { type: 'bytes', id: 2 },
          },
        },
        ReferentialIntegrityConflictDetail: {
          fields: {
            target_artifact_id: { type: 'uint64', id: 1 },
            reference_key_type: { type: 'string', id: 2 },
            referencing_artifact_ids: { rule: 'repeated', type: 'uint64', id: 3 },
          },
        },
        SnapshotTransactionError: {
          fields: {
            category: { type: 'string', id: 1 },
            description: { type: 'string', id: 2 },
            id: { type: 'string', id: 3 },
          },
        },
        FetchIndexError: {
          fields: {
            category: { type: 'string', id: 1 },
            description: { type: 'string', id: 2 },
            key_type: { type: 'string', id: 3 },
          },
        },
        RegisterTypeVersionError: {
          fields: {
            violations: { rule: 'repeated', type: 'TypeRegistrationViolation', id: 1 },
          },
        },
        TypeRegistrationViolation: {
          fields: {
            category: { type: 'string', id: 1 },
            description: { type: 'string', id: 2 },
            subject: { type: 'string', id: 3 },
          },
        },
      },
    },
  },
};

const _error_root = protobuf.Root.fromJSON(_error_proto_json);

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
  IndexFetchError,
  TypeRegistrationError,
  parse_grpc_error,
  // Exported for testing
  decode_rpc_status,
  extract_type_name,
  _error_root,
};

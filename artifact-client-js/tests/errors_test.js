"use strict";

const { describe, it } = require('node:test');
const assert = require('node:assert');
const grpc = require('@grpc/grpc-js');
const protobuf = require('protobufjs');
const {
  ArtifactError,
  ArtifactNotFoundError,
  WriteValidationError,
  ConflictError,
  TransactionError,
  TransactionSettledError,
  IndexFetchError,
  TypeRegistrationError,
  parse_grpc_error,
  decode_rpc_status,
  extract_type_name,
  _error_root,
} = require('../lib/errors');

/**
 * Encode a google.rpc.Status containing a single Any-wrapped detail message.
 */
function encode_status_with_detail (grpc_code, message, type_name, detail_obj) {
  const MessageType = _error_root.lookupType(type_name);
  const detail_bytes = MessageType.encode(MessageType.fromObject(detail_obj)).finish();

  const type_url = `type.googleapis.com/${type_name}`;

  // Manually encode google.protobuf.Any: field 1 = type_url, field 2 = value
  const any_writer = protobuf.Writer.create();
  any_writer.uint32(10).string(type_url); // field 1, wire type 2
  any_writer.uint32(18).bytes(detail_bytes); // field 2, wire type 2
  const any_bytes = any_writer.finish();

  // Manually encode google.rpc.Status: field 1 = code, field 2 = message, field 3 = details
  const status_writer = protobuf.Writer.create();
  status_writer.uint32(8).int32(grpc_code); // field 1, wire type 0
  status_writer.uint32(18).string(message); // field 2, wire type 2
  status_writer.uint32(26).bytes(any_bytes); // field 3, wire type 2

  return status_writer.finish();
}

function make_grpc_error (code, message, status_bytes) {
  const err = new Error(message);
  err.code = code;
  err.metadata = new grpc.Metadata();
  if (status_bytes) {
    err.metadata.set('grpc-status-details-bin', Buffer.from(status_bytes));
  }
  return err;
}

describe('extract_type_name', () => {
  it('extracts type name from full type_url', () => {
    assert.strictEqual(
      extract_type_name('type.googleapis.com/artifact_system.ArtifactNotFoundError'),
      'artifact_system.ArtifactNotFoundError',
    );
  });

  it('handles type_url without slash', () => {
    assert.strictEqual(
      extract_type_name('artifact_system.ArtifactNotFoundError'),
      'artifact_system.ArtifactNotFoundError',
    );
  });
});

describe('decode_rpc_status', () => {
  it('decodes a status with code, message, and details', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.NOT_FOUND,
      'not found',
      'artifact_system.ArtifactNotFoundError',
      { artifact_id: 42, tombstoned: false },
    );
    const status = decode_rpc_status(status_bytes);
    assert.strictEqual(status.code, grpc.status.NOT_FOUND);
    assert.strictEqual(status.message, 'not found');
    assert.strictEqual(status.details.length, 1);
    assert.ok(status.details[0].type_url.includes('ArtifactNotFoundError'));
  });
});

describe('parse_grpc_error', () => {
  it('parses ArtifactNotFoundError', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.NOT_FOUND,
      'artifact not found',
      'artifact_system.ArtifactNotFoundError',
      { artifact_id: 123, tombstoned: true },
    );
    const grpc_err = make_grpc_error(grpc.status.NOT_FOUND, 'artifact not found', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof ArtifactNotFoundError);
      assert.ok(err instanceof ArtifactError);
      assert.strictEqual(err.code, grpc.status.NOT_FOUND);
      assert.strictEqual(err.detail.artifact_id, '123');
      assert.strictEqual(err.detail.tombstoned, true);
      assert.strictEqual(err.grpc_error, grpc_err);
      return true;
    });
  });

  it('parses WriteValidationError (ArtifactWriteError)', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.INVALID_ARGUMENT,
      'validation failed',
      'artifact_system.ArtifactWriteError',
      {
        violations: [
          { category: 'MUTATION_DENIED', description: 'type denies create', subject: 'MyType' },
        ],
      },
    );
    const grpc_err = make_grpc_error(grpc.status.INVALID_ARGUMENT, 'validation failed', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof WriteValidationError);
      assert.strictEqual(err.detail.violations.length, 1);
      assert.strictEqual(err.detail.violations[0].category, 'MUTATION_DENIED');
      return true;
    });
  });

  it('parses ConflictError (CommitConflict)', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.ABORTED,
      'commit conflict',
      'artifact_system.CommitConflict',
      {
        conflict_type: 'INDEX_CONFLICT',
        retryable: true,
        attempts: 3,
      },
    );
    const grpc_err = make_grpc_error(grpc.status.ABORTED, 'commit conflict', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof ConflictError);
      assert.strictEqual(err.detail.conflict_type, 'INDEX_CONFLICT');
      assert.strictEqual(err.detail.retryable, true);
      assert.strictEqual(err.detail.attempts, 3);
      return true;
    });
  });

  it('parses TransactionError (SnapshotTransactionError)', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.NOT_FOUND,
      'transaction not found',
      'artifact_system.SnapshotTransactionError',
      { category: 'TRANSACTION_NOT_FOUND', description: 'gone', id: 'txn-1' },
    );
    const grpc_err = make_grpc_error(grpc.status.NOT_FOUND, 'transaction not found', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof TransactionError);
      assert.strictEqual(err.detail.category, 'TRANSACTION_NOT_FOUND');
      assert.strictEqual(err.detail.id, 'txn-1');
      return true;
    });
  });

  it('parses IndexFetchError (FetchIndexError)', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.NOT_FOUND,
      'index not found',
      'artifact_system.FetchIndexError',
      { category: 'INDEX_NOT_FOUND', description: 'no such index', key_type: 'my_key' },
    );
    const grpc_err = make_grpc_error(grpc.status.NOT_FOUND, 'index not found', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof IndexFetchError);
      assert.strictEqual(err.detail.category, 'INDEX_NOT_FOUND');
      assert.strictEqual(err.detail.key_type, 'my_key');
      return true;
    });
  });

  it('parses TypeRegistrationError (RegisterTypeVersionError)', () => {
    const status_bytes = encode_status_with_detail(
      grpc.status.INVALID_ARGUMENT,
      'registration failed',
      'artifact_system.RegisterTypeVersionError',
      {
        violations: [
          { category: 'PROTO_COMPILATION_FAILURE', description: 'bad proto', subject: 'Foo' },
        ],
      },
    );
    const grpc_err = make_grpc_error(grpc.status.INVALID_ARGUMENT, 'registration failed', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof TypeRegistrationError);
      assert.strictEqual(err.detail.violations.length, 1);
      assert.strictEqual(err.detail.violations[0].category, 'PROTO_COMPILATION_FAILURE');
      return true;
    });
  });

  it('falls back to generic ArtifactError for unknown detail types', () => {
    // Encode a status with an unrecognized type_url
    const status_writer = protobuf.Writer.create();
    status_writer.uint32(8).int32(grpc.status.INTERNAL);
    status_writer.uint32(18).string('internal error');
    // Any with unknown type
    const any_writer = protobuf.Writer.create();
    any_writer.uint32(10).string('type.googleapis.com/unknown.Type');
    any_writer.uint32(18).bytes(Buffer.alloc(0));
    status_writer.uint32(26).bytes(any_writer.finish());

    const status_bytes = status_writer.finish();
    const grpc_err = make_grpc_error(grpc.status.INTERNAL, 'internal error', status_bytes);

    assert.throws(() => parse_grpc_error(grpc_err), (err) => {
      assert.ok(err instanceof ArtifactError);
      assert.ok(!(err instanceof ArtifactNotFoundError));
      assert.strictEqual(err.detail, null);
      return true;
    });
  });

  it('falls back to generic ArtifactError when no metadata', () => {
    const err = new Error('connection refused');
    err.code = grpc.status.UNAVAILABLE;
    err.metadata = new grpc.Metadata();

    assert.throws(() => parse_grpc_error(err), (thrown) => {
      assert.ok(thrown instanceof ArtifactError);
      assert.strictEqual(thrown.code, grpc.status.UNAVAILABLE);
      assert.strictEqual(thrown.detail, null);
      return true;
    });
  });

  it('falls back to generic ArtifactError when metadata is null', () => {
    const err = new Error('no metadata');
    err.code = grpc.status.INTERNAL;

    assert.throws(() => parse_grpc_error(err), (thrown) => {
      assert.ok(thrown instanceof ArtifactError);
      return true;
    });
  });
});

describe('TransactionSettledError', () => {
  it('is thrown client-side with no gRPC detail', () => {
    const err = new TransactionSettledError();
    assert.strictEqual(err.name, 'TransactionSettledError');
    assert.strictEqual(err.message, 'Transaction has already been settled');
    assert.ok(err instanceof Error);
    assert.ok(!(err instanceof ArtifactError));
  });

  it('accepts a custom message', () => {
    const err = new TransactionSettledError('custom msg');
    assert.strictEqual(err.message, 'custom msg');
  });
});

describe('error class hierarchy', () => {
  it('all error classes extend ArtifactError', () => {
    const classes = [
      ArtifactNotFoundError,
      WriteValidationError,
      ConflictError,
      TransactionError,
      IndexFetchError,
      TypeRegistrationError,
    ];
    for (const Cls of classes) {
      const err = new Cls('test', 0, null, null);
      assert.ok(err instanceof ArtifactError, `${Cls.name} should extend ArtifactError`);
      assert.ok(err instanceof Error, `${Cls.name} should extend Error`);
    }
  });
});

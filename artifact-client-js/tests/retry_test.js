"use strict";

const { describe, it } = require('node:test');
const assert = require('node:assert');
const grpc = require('@grpc/grpc-js');
const { retry_with_backoff, is_retryable } = require('../lib/retry');

describe('retry_with_backoff', () => {
  it('returns on first success without retrying', async () => {
    let call_count = 0;
    const result = await retry_with_backoff(async () => {
      call_count++;
      return 'ok';
    }, { max_retries: 3 });

    assert.strictEqual(result, 'ok');
    assert.strictEqual(call_count, 1);
  });

  it('retries on UNAVAILABLE and succeeds', async () => {
    let call_count = 0;
    const result = await retry_with_backoff(async () => {
      call_count++;
      if (call_count < 3) {
        const err = new Error('unavailable');
        err.code = grpc.status.UNAVAILABLE;
        throw err;
      }
      return 'recovered';
    }, { max_retries: 5, base_delay_ms: 1, max_delay_ms: 2 });

    assert.strictEqual(result, 'recovered');
    assert.strictEqual(call_count, 3);
  });

  it('retries on DEADLINE_EXCEEDED and succeeds', async () => {
    let call_count = 0;
    const result = await retry_with_backoff(async () => {
      call_count++;
      if (call_count < 2) {
        const err = new Error('deadline');
        err.code = grpc.status.DEADLINE_EXCEEDED;
        throw err;
      }
      return 'ok';
    }, { max_retries: 3, base_delay_ms: 1, max_delay_ms: 2 });

    assert.strictEqual(result, 'ok');
    assert.strictEqual(call_count, 2);
  });

  it('does not retry non-retryable errors', async () => {
    let call_count = 0;
    await assert.rejects(async () => {
      await retry_with_backoff(async () => {
        call_count++;
        const err = new Error('not found');
        err.code = grpc.status.NOT_FOUND;
        throw err;
      }, { max_retries: 3, base_delay_ms: 1 });
    }, (err) => {
      assert.strictEqual(err.code, grpc.status.NOT_FOUND);
      return true;
    });

    assert.strictEqual(call_count, 1);
  });

  it('throws last error after exhausting retries', async () => {
    let call_count = 0;
    await assert.rejects(async () => {
      await retry_with_backoff(async () => {
        call_count++;
        const err = new Error(`fail ${call_count}`);
        err.code = grpc.status.UNAVAILABLE;
        throw err;
      }, { max_retries: 2, base_delay_ms: 1, max_delay_ms: 2 });
    }, (err) => {
      assert.strictEqual(err.message, 'fail 3');
      return true;
    });

    assert.strictEqual(call_count, 3); // initial + 2 retries
  });
});

describe('is_retryable', () => {
  it('returns true for UNAVAILABLE', () => {
    assert.strictEqual(is_retryable({ code: grpc.status.UNAVAILABLE }), true);
  });

  it('returns true for DEADLINE_EXCEEDED', () => {
    assert.strictEqual(is_retryable({ code: grpc.status.DEADLINE_EXCEEDED }), true);
  });

  it('returns false for NOT_FOUND', () => {
    assert.strictEqual(is_retryable({ code: grpc.status.NOT_FOUND }), false);
  });

  it('returns false for null/undefined', () => {
    assert.strictEqual(is_retryable(null), false);
    assert.strictEqual(is_retryable(undefined), false);
  });
});

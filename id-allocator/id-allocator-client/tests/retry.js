"use strict";

const assert = require('node:assert');
const { describe, it } = require('node:test');
const { retry_with_backoff } = require('../lib/retry');

describe('retry_with_backoff', () => {
  it('returns immediately on success', async () => {
    let call_count = 0;
    const result = await retry_with_backoff(() => {
      call_count++;
      return Promise.resolve('ok');
    });

    assert.strictEqual(result, 'ok');
    assert.strictEqual(call_count, 1);
  });

  it('retries and succeeds after transient failures', async () => {
    let call_count = 0;
    const result = await retry_with_backoff(() => {
      call_count++;
      if (call_count < 3) {
        return Promise.reject(new Error('transient'));
      }
      return Promise.resolve('recovered');
    }, { base_delay_ms: 1, max_delay_ms: 10 });

    assert.strictEqual(result, 'recovered');
    assert.strictEqual(call_count, 3);
  });

  it('throws after exhausting all retries', async () => {
    let call_count = 0;
    await assert.rejects(
      () => retry_with_backoff(() => {
        call_count++;
        return Promise.reject(new Error('permanent'));
      }, { max_retries: 2, base_delay_ms: 1, max_delay_ms: 5 }),
      /permanent/,
    );

    // 1 initial + 2 retries = 3 calls
    assert.strictEqual(call_count, 3);
  });

  it('respects max_retries of 0 (no retries)', async () => {
    let call_count = 0;
    await assert.rejects(
      () => retry_with_backoff(() => {
        call_count++;
        return Promise.reject(new Error('fail'));
      }, { max_retries: 0 }),
      /fail/,
    );

    assert.strictEqual(call_count, 1);
  });

  it('passes through the last error', async () => {
    let call_count = 0;
    await assert.rejects(
      () => retry_with_backoff(() => {
        call_count++;
        return Promise.reject(new Error(`error-${call_count}`));
      }, { max_retries: 2, base_delay_ms: 1, max_delay_ms: 5 }),
      /error-3/,
    );
  });
});

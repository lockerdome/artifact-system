"use strict";

const grpc = require('@grpc/grpc-js');

const DEFAULT_BASE_DELAY_MS = 100;
const DEFAULT_MAX_DELAY_MS = 10000;
const DEFAULT_MAX_RETRIES = 5;

const RETRYABLE_STATUS_CODES = new Set([
  grpc.status.UNAVAILABLE,
  grpc.status.DEADLINE_EXCEEDED,
]);

function is_retryable (err) {
  return err != null && RETRYABLE_STATUS_CODES.has(err.code);
}

/**
 * Retries an async function with exponential backoff and jitter.
 * Only retries on transient gRPC errors (UNAVAILABLE, DEADLINE_EXCEEDED).
 *
 * @param {function} fn - Async function to retry.
 * @param {object} [options]
 * @param {number} [options.max_retries=5]
 * @param {number} [options.base_delay_ms=100]
 * @param {number} [options.max_delay_ms=10000]
 * @returns {Promise<*>}
 */
async function retry_with_backoff (fn, options = {}) {
  const max_retries = options.max_retries ?? DEFAULT_MAX_RETRIES;
  const base_delay_ms = options.base_delay_ms ?? DEFAULT_BASE_DELAY_MS;
  const max_delay_ms = options.max_delay_ms ?? DEFAULT_MAX_DELAY_MS;

  let last_error;

  for (let attempt = 0; attempt <= max_retries; ++attempt) {
    try {
      return await fn();
    } catch (err) {
      last_error = err;

      if (attempt === max_retries || !is_retryable(err)) {
        break;
      }

      const exponential_delay = base_delay_ms * Math.pow(2, attempt);
      const capped_delay = Math.min(exponential_delay, max_delay_ms);
      const jittered_delay = Math.random() * capped_delay;

      await new Promise(resolve => setTimeout(resolve, jittered_delay));
    }
  }

  throw last_error;
}

module.exports = { retry_with_backoff, is_retryable, RETRYABLE_STATUS_CODES };

"use strict";

const DEFAULT_BASE_DELAY_MS = 100;
const DEFAULT_MAX_DELAY_MS = 10000;
const DEFAULT_MAX_RETRIES = 5;

/**
 * Retries an async function with exponential backoff and jitter.
 *
 * @param {function} fn - Async function to retry.
 * @param {object} [options]
 * @param {number} [options.max_retries=5] - Maximum number of retry attempts.
 * @param {number} [options.base_delay_ms=100] - Base delay for the first retry.
 * @param {number} [options.max_delay_ms=10000] - Maximum delay between retries.
 * @returns {Promise<*>} The result of fn on success.
 * @throws The last error if all retries are exhausted.
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

      if (attempt === max_retries) {
        break;
      }

      // Exponential backoff: base * 2^attempt, capped at max_delay_ms.
      const exponential_delay = base_delay_ms * Math.pow(2, attempt);
      const capped_delay = Math.min(exponential_delay, max_delay_ms);

      // Full jitter: uniform random in [0, capped_delay].
      const jittered_delay = Math.random() * capped_delay;

      await new Promise(resolve => setTimeout(resolve, jittered_delay));
    }
  }

  throw last_error;
}

module.exports = { retry_with_backoff };

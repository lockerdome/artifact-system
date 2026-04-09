"use strict";

const { spawn } = require('child_process');
const path = require('path');

const DEFAULT_BINARY_PATH = path.resolve(
  __dirname, '..', '..', '..', '..', 'artifact-system', 'artifact-layer',
  'build', 'release', 'artifact_layer_service',
);

/**
 * Spawn the artifact-layer C++ server on an ephemeral port and wait for
 * it to print the LISTENING_PORT=<n> line before resolving.
 *
 * The binary path defaults to the cmake release build output; override
 * with the ARTIFACT_LAYER_BINARY_PATH environment variable.
 *
 * @returns {Promise<{ address: string, process: ChildProcess, stop: () => Promise<void> }>}
 */
function start_server () {
  const binary_path = process.env.ARTIFACT_LAYER_BINARY_PATH || DEFAULT_BINARY_PATH;

  return new Promise((resolve, reject) => {
    const server_process = spawn(binary_path, [], {
      env: {
        ...process.env,
        ARTIFACT_LAYER_LISTEN_ADDRESS: '127.0.0.1:0',
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    });

    let stdout_buffer = '';
    let resolved = false;

    server_process.stdout.on('data', (data) => {
      stdout_buffer += data.toString();
      if (!resolved) {
        const match = stdout_buffer.match(/^LISTENING_PORT=(\d+)/m);
        if (match) {
          resolved = true;
          const port = parseInt(match[1], 10);
          const address = `127.0.0.1:${port}`;
          resolve({
            address,
            process: server_process,
            stop () {
              return new Promise((res) => {
                if (server_process.exitCode !== null) return res();
                server_process.once('exit', () => res());
                server_process.kill('SIGTERM');
              });
            },
          });
        }
      }
    });

    let stderr_buffer = '';
    server_process.stderr.on('data', (data) => {
      stderr_buffer += data.toString();
    });

    server_process.on('error', (err) => {
      if (!resolved) {
        reject(new Error(`Failed to spawn artifact-layer server at ${binary_path}: ${err.message}`));
      }
    });

    server_process.on('exit', (code) => {
      if (!resolved) {
        reject(new Error(
          `artifact-layer server exited (code ${code}) before reporting ` +
          `LISTENING_PORT.\nstdout: ${stdout_buffer}\nstderr: ${stderr_buffer}`,
        ));
      }
    });
  });
}

module.exports = { start_server };

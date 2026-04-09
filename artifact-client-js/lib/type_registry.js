"use strict";

const protobuf = require('protobufjs');
const { TypeDecodeError, _error_root } = require('./errors');

/**
 * Built-in TypeDefinition message used to resolve a `version_id` to its
 * `type_name`.  The artifact_service.desc descriptor set built by
 * `npm run generate-proto` includes artifact_types.proto, which contains
 * the TypeDefinition message definition.
 */
const _TypeDefinitionType = _error_root.lookupType('artifact_system.TypeDefinition');

/**
 * Decode a TypeDefinition payload (raw bytes from a GetArtifact response on
 * a TypeDefinition artifact) and return the JS object form.
 */
function _decode_type_definition (payload_bytes) {
  const decoded = _TypeDefinitionType.decode(payload_bytes);
  return _TypeDefinitionType.toObject(decoded, {
    longs: String,
    enums: String,
    defaults: true,
  });
}

/**
 * TypeRegistryCache resolves and caches protobuf message types for artifact
 * payloads and index payloads.  A single instance is shared across an
 * ArtifactClient's Snapshot and Transaction objects.
 *
 * Caches:
 *   - _version_root_cache:    version_id (string) → Promise<protobufjs.Root>
 *   - _version_type_id_cache: version_id (string) → Promise<string> (type_id)
 *   - _version_type_name_cache: version_id (string) → Promise<string> (type_name)
 *   - _type_definition_name_cache: type_id (string) → string (type_name)
 *   - _index_schema_cache:    key_type (string) → Promise<{ root, key_message_name,
 *                                                   index_message_name }>
 *
 * version_ids and type_ids are immutable in the artifact system, so the type
 * caches never need invalidation.  The index schema cache is keyed on
 * key_type and is valid for the lifetime of the client, since index schemas
 * don't change within a running process.
 */
class TypeRegistryCache {
  /**
   * @param {import('./grpc_client').ArtifactGrpcClient} grpc_client
   */
  constructor (grpc_client) {
    this._grpc_client = grpc_client;
    this._version_root_cache = new Map();
    this._version_type_id_cache = new Map();
    this._version_type_name_cache = new Map();
    this._type_definition_name_cache = new Map();
    this._index_schema_cache = new Map();
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Type resolution (artifact payloads)
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Resolve a `version_id` to a protobufjs Root containing all messages
   * declared in that version's descriptor set.  Cached after first lookup.
   * The cache stores promises so concurrent callers share one in-flight RPC.
   *
   * @param {string} version_id
   * @param {object} read_context  ReadContext used for the GetTypeVersion call.
   * @returns {Promise<protobuf.Root>}
   */
  async _get_root_for_version (version_id, read_context) {
    const cached = this._version_root_cache.get(version_id);
    if (cached) return cached;

    const promise = this._fetch_root_for_version(version_id, read_context);
    this._version_root_cache.set(version_id, promise);
    try {
      return await promise;
    } catch (err) {
      this._version_root_cache.delete(version_id);
      throw err;
    }
  }

  /**
   * Fetch the protobufjs Root for a version_id from the server.  Also
   * opportunistically caches the type_id from the response so that
   * _get_type_name_for_version can skip a redundant RPC.
   *
   * @param {string} version_id
   * @param {object} read_context
   * @returns {Promise<protobuf.Root>}
   */
  async _fetch_root_for_version (version_id, read_context) {
    const response = await this._grpc_client.get_type_version({
      version_id,
      read_context,
    });

    if (!response.descriptor_set) {
      throw new TypeDecodeError(
        `GetTypeVersion(${version_id}) returned no descriptor_set`,
      );
    }

    // get_type_version uses protobufjs-based serialization (bypassing
    // proto-loader), so response.descriptor_set is a protobufjs Message
    // with no defaults pollution.  Re-encode to bytes so
    // Root.fromDescriptor can decode internally with its own (camelCase)
    // descriptor walker.
    const fds_type = response.descriptor_set.constructor;
    const fds_bytes = fds_type.encode(response.descriptor_set).finish();
    const root = protobuf.Root.fromDescriptor(fds_bytes);

    // Opportunistically cache the type_id so _get_type_name_for_version
    // can skip a redundant GetTypeVersion RPC.
    if (response.type_id != null) {
      this._version_type_id_cache.set(version_id, Promise.resolve(String(response.type_id)));
    }

    return root;
  }

  /**
   * Resolve `version_id` → `type_name`.  This requires a second lookup
   * because GetTypeVersionResponse does not currently carry type_name; we
   * fetch the parent TypeDefinition artifact (whose artifact_id == type_id)
   * and read its `type_name` field.
   *
   * Both `version_id → type_name` and `type_id → type_name` are cached.
   * The cache stores promises so concurrent callers share one in-flight fetch.
   *
   * @param {string} version_id
   * @param {object} read_context
   * @returns {Promise<string>}
   */
  async _get_type_name_for_version (version_id, read_context) {
    const cached = this._version_type_name_cache.get(version_id);
    if (cached) return cached;

    const promise = this._fetch_type_name_for_version(version_id, read_context);
    this._version_type_name_cache.set(version_id, promise);
    try {
      return await promise;
    } catch (err) {
      this._version_type_name_cache.delete(version_id);
      throw err;
    }
  }

  /**
   * Fetch the type_name for a version_id from the server.  Uses the
   * _version_type_id_cache (populated by _fetch_root_for_version) to avoid
   * a redundant GetTypeVersion RPC when the type_id is already known.
   *
   * @param {string} version_id
   * @param {object} read_context
   * @returns {Promise<string>}
   */
  async _fetch_type_name_for_version (version_id, read_context) {
    // Check if _fetch_root_for_version already cached the type_id.
    let type_id_str;
    const cached_type_id = this._version_type_id_cache.get(version_id);
    if (cached_type_id) {
      type_id_str = await cached_type_id;
    } else {
      // GetTypeVersion gives us the type_id for this version.
      const version_response = await this._grpc_client.get_type_version({
        version_id,
        read_context,
      });
      const type_id = version_response.type_id;
      if (type_id == null) {
        throw new TypeDecodeError(
          `GetTypeVersion(${version_id}) returned no type_id`,
        );
      }
      type_id_str = String(type_id);
    }

    let type_name = this._type_definition_name_cache.get(type_id_str);
    if (!type_name) {
      const type_def_artifact = await this._grpc_client.get_artifact({
        artifact_id: type_id_str,
        context: read_context,
      });
      const type_def = _decode_type_definition(type_def_artifact.payload);
      type_name = type_def.type_name;
      if (!type_name) {
        throw new TypeDecodeError(
          `TypeDefinition artifact ${type_id_str} has no type_name`,
        );
      }
      this._type_definition_name_cache.set(type_id_str, type_name);
    }

    return type_name;
  }

  /**
   * Resolve a (version_id, type_name) pair to the protobufjs Type used to
   * encode/decode payload bytes.  Throws TypeDecodeError if the type is not
   * present in the version's descriptor set.
   *
   * @param {string} version_id
   * @param {string} type_name  Fully-qualified message name (with package).
   * @param {object} read_context
   * @returns {Promise<protobuf.Type>}
   */
  async resolve_artifact_type (version_id, type_name, read_context) {
    const root = await this._get_root_for_version(version_id, read_context);
    let message_type;
    try {
      message_type = root.lookupType(type_name);
    } catch (_) {
      throw new TypeDecodeError(
        `Type "${type_name}" not found in descriptor set for version_id ${version_id}`,
      );
    }
    return message_type;
  }

  /**
   * Decode raw payload bytes for an artifact into a JS object.
   *
   * @param {string} version_id
   * @param {string} type_name
   * @param {Uint8Array|Buffer} payload_bytes
   * @param {object} read_context
   * @returns {Promise<object>}
   */
  async decode_artifact_payload (version_id, type_name, payload_bytes, read_context) {
    const message_type = await this.resolve_artifact_type(
      version_id, type_name, read_context,
    );
    const decoded = message_type.decode(payload_bytes);
    return message_type.toObject(decoded, {
      longs: String,
      enums: String,
      defaults: true,
    });
  }

  /**
   * Encode a JS object into payload bytes for an artifact.  The user only
   * supplies `version_id` and the payload object; we look up `type_name`
   * via the parent TypeDefinition.
   *
   * @param {string} version_id
   * @param {object} payload_object
   * @param {object} read_context
   * @returns {Promise<{type_name: string, payload_bytes: Uint8Array}>}
   */
  async encode_artifact_payload (version_id, payload_object, read_context) {
    const type_name = await this._get_type_name_for_version(version_id, read_context);
    const message_type = await this.resolve_artifact_type(
      version_id, type_name, read_context,
    );
    // Note: we deliberately skip verify() here.  protobufjs's verify() is
    // strict about numeric types (e.g. uint64 fields require integer|Long)
    // but this client represents all IDs as strings (longs: String).
    // fromObject() handles string → Long conversion natively.
    const message = message_type.fromObject(payload_object);
    const payload_bytes = message_type.encode(message).finish();
    return { type_name, payload_bytes };
  }

  // ─────────────────────────────────────────────────────────────────────────
  // Index schema resolution
  // ─────────────────────────────────────────────────────────────────────────

  /**
   * Resolve an index `key_type` to its protobufjs types for the index key
   * and index value messages.  Cached after first lookup.  The cache stores
   * promises so concurrent callers share one in-flight RPC.
   *
   * The cache is keyed on key_type and is valid for the lifetime of the
   * client, since index schemas don't change within a running process.
   *
   * @param {string} key_type
   * @param {object} read_context
   * @returns {Promise<{
   *   root: protobuf.Root,
   *   key_message_name: string,
   *   index_message_name: string,
   *   key_type_msg: protobuf.Type,
   *   index_type_msg: protobuf.Type,
   * }>}
   */
  async _get_index_schema (key_type, read_context) {
    const cached = this._index_schema_cache.get(key_type);
    if (cached) return cached;

    const promise = this._fetch_index_schema(key_type, read_context);
    this._index_schema_cache.set(key_type, promise);
    try {
      return await promise;
    } catch (err) {
      this._index_schema_cache.delete(key_type);
      throw err;
    }
  }

  /**
   * Fetch the index schema for a key_type from the server.
   *
   * @param {string} key_type
   * @param {object} read_context
   * @returns {Promise<{
   *   root: protobuf.Root,
   *   key_message_name: string,
   *   index_message_name: string,
   *   key_type_msg: protobuf.Type,
   *   index_type_msg: protobuf.Type,
   * }>}
   */
  async _fetch_index_schema (key_type, read_context) {
    const response = await this._grpc_client.get_index_schema({
      key_type,
      read_context,
    });

    if (!response.index_descriptor_set) {
      throw new TypeDecodeError(
        `GetIndexSchema(${key_type}) returned no index_descriptor_set`,
      );
    }

    const fds_type = response.index_descriptor_set.constructor;
    const fds_bytes = fds_type.encode(response.index_descriptor_set).finish();
    const root = protobuf.Root.fromDescriptor(fds_bytes);

    let key_type_msg;
    try {
      key_type_msg = root.lookupType(response.key_message_name);
    } catch (_) {
      throw new TypeDecodeError(
        `Index key message "${response.key_message_name}" not found in ` +
        `descriptor set for key_type ${key_type}`,
      );
    }

    let index_type_msg;
    try {
      index_type_msg = root.lookupType(response.index_message_name);
    } catch (_) {
      throw new TypeDecodeError(
        `Index message "${response.index_message_name}" not found in ` +
        `descriptor set for key_type ${key_type}`,
      );
    }

    return {
      root,
      key_message_name: response.key_message_name,
      index_message_name: response.index_message_name,
      key_type_msg,
      index_type_msg,
    };
  }

  /**
   * Encode a JS key object into the bytes expected by the FetchIndex RPC.
   *
   * @param {string} key_type
   * @param {object} key_object
   * @param {object} read_context
   * @returns {Promise<Uint8Array>}
   */
  async encode_index_key (key_type, key_object, read_context) {
    const schema = await this._get_index_schema(key_type, read_context);
    // Skip verify() — same reasoning as encode_artifact_payload: uint64
    // fields are strings in this client, and fromObject() handles the
    // string → Long conversion.
    const message = schema.key_type_msg.fromObject(key_object);
    return schema.key_type_msg.encode(message).finish();
  }

  /**
   * Decode the index payload bytes returned by FetchIndex.  Uses the
   * `index_message_name` from the response (which is also stored on the
   * schema cache and verified to match).
   *
   * @param {string} key_type
   * @param {string} index_message_name  From FetchIndexResponse.
   * @param {Uint8Array|Buffer} index_payload_bytes
   * @param {object} read_context
   * @returns {Promise<object>}
   */
  async decode_index_payload (key_type, index_message_name, index_payload_bytes, read_context) {
    const schema = await this._get_index_schema(key_type, read_context);

    // Sanity check: the server should return the same index_message_name as
    // the schema we cached.  If they diverge, the cached schema is stale.
    let index_type_msg = schema.index_type_msg;
    if (index_message_name && index_message_name !== schema.index_message_name) {
      try {
        index_type_msg = schema.root.lookupType(index_message_name);
      } catch (_) {
        throw new TypeDecodeError(
          `FetchIndex returned index_message_name "${index_message_name}" ` +
          `not present in cached descriptor set for key_type ${key_type}`,
        );
      }
    }

    const decoded = index_type_msg.decode(index_payload_bytes);
    return index_type_msg.toObject(decoded, {
      longs: String,
      enums: String,
      defaults: true,
    });
  }
}

module.exports = { TypeRegistryCache };

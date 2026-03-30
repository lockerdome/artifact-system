#pragma once

// Internal bypass of mutation restrictions.
//
// Certain artifact layer operations require creating, updating, or deleting
// artifacts whose types have deny_create, deny_update, or deny_delete set.
// For example, RegisterTypeVersion creates TypeVersionDefinition and
// IndexDefinition artifacts (bypassing deny_create), updates the tail
// TypeVersionDefinition's next_version_id (bypassing deny_update), and updates
// TypeDefinition's current_version_id and mutation-restriction flags (bypassing
// deny_update).
//
// The bypass is architectural: TypeRegistry holds a private ArtifactStore
// constructed with bypass_mutation_check=true. External callers never have
// access to this store instance. The InternalBypassToken is a non-copyable,
// non-default-constructible marker that TypeRegistry passes to any helper
// that needs to assert it is operating in a bypass context.

namespace artifact_system::registry {

// Non-copyable marker token proving the caller has bypass authority.
// Only TypeRegistry can construct one (via friendship).
class InternalBypassToken {
public:
  InternalBypassToken(const InternalBypassToken&) = delete;
  InternalBypassToken& operator=(const InternalBypassToken&) = delete;
  InternalBypassToken(InternalBypassToken&&) = default;
  InternalBypassToken& operator=(InternalBypassToken&&) = default;

private:
  friend class TypeRegistry;
  InternalBypassToken() = default;
};

} // namespace artifact_system::registry

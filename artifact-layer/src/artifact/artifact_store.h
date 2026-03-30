#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include "google/protobuf/descriptor.pb.h"

#include "artifact/referential_integrity.h"
#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "id/id_allocator_interface.h"
#include "index/index_derivation.h"
#include "storage/storage_interface.h"
#include "transaction/transaction_manager.h"
#include "transaction/write_executor.h"

namespace artifact_system::artifact {

// Result of a successful write operation (Create/Update/Delete).
struct WriteResult {
  std::string snapshot_id;
};

// Result of a successful CreateArtifact.
struct CreateResult {
  uint64_t artifact_id = 0;
  std::string snapshot_id;
};

// Result of GetArtifact.
struct GetResult {
  uint64_t artifact_id = 0;
  std::string type_name;
  uint64_t version_id = 0;
  std::string payload;
};

// Per-ID result for BatchGetArtifacts.
struct BatchGetEntry {
  std::variant<GetResult, ArtifactNotFoundError> result;
};

// ArtifactStore provides the core CRUD engine for artifacts.
//
// It coordinates validation, index derivation, referential integrity,
// and transactional writes.  Reads can be scoped to a snapshot,
// transaction, or the canonical branch head.
class ArtifactStore {
public:
  struct Options {
    // Map from index key_type to IndexDefinition artifact_id.
    // Required for index derivation and referential integrity lookups.
    std::unordered_map<std::string, uint64_t> index_def_ids_by_key_type;
    // When true, bypass mutation restriction checks (for internal operations).
    bool bypass_mutation_check = false;
    // When true, bypass referential integrity validation (for internal operations
    // like type registration that manage system artifacts with special naming).
    bool bypass_referential_integrity = false;
  };

  ArtifactStore(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator);
  ArtifactStore(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator, Options options);

  // ── Write operations ──────────────────────────────────────────────────────

  // CreateArtifact: allocate ID, validate, derive indexes, stage writes.
  // If transaction_id is set, writes into that transaction.
  // Otherwise, wraps in an implicit transaction.
  absl::StatusOr<CreateResult> CreateArtifact(uint64_t version_id, const std::string& payload,
                                              const std::optional<std::string>& transaction_id = std::nullopt);

  // UpdateArtifact: validate type match, revalidate payload, compute index diff.
  absl::StatusOr<WriteResult> UpdateArtifact(uint64_t artifact_id, uint64_t version_id, const std::string& payload,
                                             const std::optional<std::string>& transaction_id = std::nullopt);

  // DeleteArtifact: write tombstone, remove indexes, enforce referential integrity.
  absl::StatusOr<WriteResult> DeleteArtifact(uint64_t artifact_id, const std::optional<std::string>& transaction_id = std::nullopt);

  // ── Read operations ───────────────────────────────────────────────────────

  // GetArtifact: read from snapshot/transaction/canonical branch.
  absl::StatusOr<GetResult> GetArtifact(uint64_t artifact_id, const ReadContext& context);

  // BatchGetArtifacts: per-id results preserving positional correlation.
  absl::StatusOr<std::vector<BatchGetEntry>> BatchGetArtifacts(const std::vector<uint64_t>& artifact_ids, const ReadContext& context);

private:
  // Resolve a ReadContext to a storage ref (branch name or commit ID).
  absl::StatusOr<std::string> ResolveReadRef(const ReadContext& context);

  // Resolve a transaction_id to its branch name, or get canonical branch.
  absl::StatusOr<std::string> ResolveWriteBranch(const std::optional<std::string>& transaction_id);

  // Execute a write operation within an explicit or implicit transaction.
  // The callback receives the branch name and should stage all writes.
  using WriteFn = std::function<absl::Status(const std::string& branch)>;
  absl::StatusOr<std::string> ExecuteWrite(const std::optional<std::string>& transaction_id, const WriteFn& write_fn);

  // Stage artifact + index writes for a create operation.
  absl::Status StageCreate(const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name, const std::string& payload,
                           const google::protobuf::FileDescriptorSet& descriptor_set);

  // Stage artifact + index writes for an update operation.
  absl::Status StageUpdate(const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name, const std::string& payload,
                           const google::protobuf::FileDescriptorSet& descriptor_set);

  // Stage tombstone + index removal for a delete operation.
  absl::Status StageDelete(const std::string& branch, uint64_t artifact_id, const StoredArtifact& existing);

  // Apply delete side effects within a write transaction.
  absl::Status ApplyCascadeEffect(const std::string& branch, const CascadeDelete& cascade);
  absl::Status ApplySetNullEffect(const std::string& branch, const SetNullUpdate& set_null);

  StorageInterface* storage_;
  transaction::TransactionManager* transaction_manager_;
  IdAllocatorInterface* id_allocator_;
  Options options_;
};

} // namespace artifact_system::artifact

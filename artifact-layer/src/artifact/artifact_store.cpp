#include "artifact/artifact_store.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact/proto_utils.h"
#include "artifact/referential_integrity.h"
#include "artifact/validation.h"
#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"
#include "index/index_utils.h"

namespace artifact_system::artifact {
namespace {

// Build an ArtifactWriteError status from violations.
absl::Status MakeWriteError(const std::vector<ArtifactWriteViolation>& violations) {
  ArtifactWriteError error;
  for (const auto& v : violations) {
    *error.add_violations() = v;
  }
  absl::Status status = absl::InvalidArgumentError("artifact write validation failed");
  // Attach the error detail as a payload.
  std::string serialized;
  error.SerializeToString(&serialized);
  status.SetPayload("type.googleapis.com/artifact_system.ArtifactWriteError", absl::Cord(serialized));
  return status;
}

// Build an ArtifactNotFoundError status.
absl::Status MakeNotFoundError(uint64_t artifact_id, bool tombstoned) {
  ArtifactNotFoundError error;
  error.set_artifact_id(artifact_id);
  error.set_tombstoned(tombstoned);
  absl::Status status = absl::NotFoundError(absl::StrCat("artifact ", artifact_id, " not found"));
  std::string serialized;
  error.SerializeToString(&serialized);
  status.SetPayload("type.googleapis.com/artifact_system.ArtifactNotFoundError", absl::Cord(serialized));
  return status;
}

// Serialize a StoredArtifact envelope.
std::string SerializeEnvelope(uint64_t version_id, const std::string& type_name, const std::string& payload) {
  StoredArtifact envelope;
  envelope.set_envelope_version(1);
  envelope.set_version_id(version_id);
  envelope.set_type_name(type_name);
  envelope.set_payload(payload);
  return envelope.SerializeAsString();
}

// Serialize a tombstone envelope.
std::string SerializeTombstone(uint64_t version_id, const std::string& type_name) {
  StoredArtifact envelope;
  envelope.set_envelope_version(1);
  envelope.set_version_id(version_id);
  envelope.set_type_name(type_name);
  // empty payload = tombstone
  return envelope.SerializeAsString();
}

// Aliases for shared index utilities used throughout this file.
using index::AddIndexRow;
using index::FindIndexDefinition;
using index::RemoveIndexRow;

// Run referential integrity validation for a create or update operation.
// Builds a dynamic message from the resolved type and validates all reference fields.
absl::StatusOr<std::vector<ArtifactWriteViolation>> RunRefIntegrityValidation(StorageInterface* storage, const std::string& read_ref,
                                                                              const ResolvedType& resolved, const std::string& payload) {
  std::vector<ArtifactWriteViolation> violations;

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(resolved.descriptor_set, resolved.type_name, &pool);
  if (descriptor == nullptr) {
    return violations;
  }

  google::protobuf::DynamicMessageFactory factory;
  const auto* prototype = factory.GetPrototype(descriptor);
  std::unique_ptr<google::protobuf::Message> msg(prototype->New());
  if (!msg->ParseFromString(payload)) {
    return violations;
  }

  RefIntegrityContext ri_ctx{storage, read_ref};
  auto ref_violations_or = ValidateReferences(*msg, *descriptor, ri_ctx);
  if (!ref_violations_or.ok()) {
    return ref_violations_or.status();
  }
  return std::move(*ref_violations_or);
}

} // namespace

ArtifactStore::ArtifactStore(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator)
    : storage_(storage), transaction_manager_(transaction_manager), id_allocator_(id_allocator) {
}

ArtifactStore::ArtifactStore(StorageInterface* storage, transaction::TransactionManager* transaction_manager, IdAllocatorInterface* id_allocator,
                             Options options)
    : storage_(storage), transaction_manager_(transaction_manager), id_allocator_(id_allocator), options_(std::move(options)) {
}

// ── Read operations ────────────��────────────────────────────────────────────

absl::StatusOr<std::string> ArtifactStore::ResolveReadRef(const ReadContext& context) {
  if (context.has_snapshot_id()) {
    // The snapshot_id IS the commit hash — validate it's known, then return directly.
    auto meta_or = transaction_manager_->GetSnapshotMetadata(context.snapshot_id());
    if (!meta_or.ok())
      return meta_or.status();
    return context.snapshot_id();
  }
  if (context.has_transaction_id()) {
    // The transaction_id IS the branch name — validate it's known, then return directly.
    auto meta_or = transaction_manager_->GetTransactionMetadata(context.transaction_id());
    if (!meta_or.ok())
      return meta_or.status();
    return context.transaction_id();
  }
  // No context: read from canonical branch head.
  return std::string(storage_->GetCanonicalBranch());
}

absl::StatusOr<GetResult> ArtifactStore::GetArtifact(uint64_t artifact_id, const ReadContext& context) {
  auto ref_or = ResolveReadRef(context);
  if (!ref_or.ok())
    return ref_or.status();

  auto stored_or = ReadStoredArtifact(storage_, *ref_or, artifact_id);
  if (!stored_or.ok()) {
    if (absl::IsNotFound(stored_or.status())) {
      return MakeNotFoundError(artifact_id, false);
    }
    return stored_or.status();
  }

  const StoredArtifact& stored = *stored_or;
  // Tombstone = not found.
  if (stored.payload().empty()) {
    return MakeNotFoundError(artifact_id, true);
  }

  GetResult result;
  result.artifact_id = artifact_id;
  result.type_name = stored.type_name();
  result.version_id = stored.version_id();
  result.payload = stored.payload();
  return result;
}

absl::StatusOr<std::vector<BatchGetEntry>> ArtifactStore::BatchGetArtifacts(const std::vector<uint64_t>& artifact_ids, const ReadContext& context) {
  auto ref_or = ResolveReadRef(context);
  if (!ref_or.ok())
    return ref_or.status();

  std::vector<BatchGetEntry> results;
  results.reserve(artifact_ids.size());

  for (const uint64_t id : artifact_ids) {
    auto stored_or = ReadStoredArtifact(storage_, *ref_or, id);
    if (!stored_or.ok()) {
      if (absl::IsNotFound(stored_or.status())) {
        ArtifactNotFoundError nfe;
        nfe.set_artifact_id(id);
        nfe.set_tombstoned(false);
        results.push_back(BatchGetEntry{nfe});
        continue;
      }
      return stored_or.status();
    }
    const StoredArtifact& stored = *stored_or;
    if (stored.payload().empty()) {
      ArtifactNotFoundError nfe;
      nfe.set_artifact_id(id);
      nfe.set_tombstoned(true);
      results.push_back(BatchGetEntry{nfe});
      continue;
    }
    GetResult gr;
    gr.artifact_id = id;
    gr.type_name = stored.type_name();
    gr.version_id = stored.version_id();
    gr.payload = stored.payload();
    results.push_back(BatchGetEntry{gr});
  }
  return results;
}

// ── Write infrastructure ────────────────────────────────────────────────────

absl::StatusOr<std::string> ArtifactStore::ResolveWriteBranch(const std::optional<std::string>& transaction_id) {
  if (transaction_id.has_value()) {
    // The transaction_id IS the branch name — validate it's known, then return directly.
    auto meta_or = transaction_manager_->GetTransactionMetadata(*transaction_id);
    if (!meta_or.ok())
      return meta_or.status();
    return *transaction_id;
  }
  return std::string(storage_->GetCanonicalBranch());
}

absl::StatusOr<std::string> ArtifactStore::ExecuteWrite(const std::optional<std::string>& transaction_id, const WriteFn& write_fn) {
  if (transaction_id.has_value()) {
    // Explicit transaction: use WriteExecutor against the transaction branch.
    // The transaction_id IS the branch name.
    auto meta_or = transaction_manager_->GetTransactionMetadata(*transaction_id);
    if (!meta_or.ok())
      return meta_or.status();

    transaction::WriteExecutor executor(storage_);
    auto result_or = executor.ExecuteWrite(*transaction_id, [&write_fn](const std::string& child_branch) { return write_fn(child_branch); });
    if (!result_or.ok())
      return result_or.status();

    auto& write_result = *result_or;
    if (std::holds_alternative<transaction::WriteExecutor::WriteConflict>(write_result)) {
      auto& conflict = std::get<transaction::WriteExecutor::WriteConflict>(write_result);
      return absl::AbortedError(absl::StrCat("write conflict after ", conflict.attempts, " attempts"));
    }

    // Create a snapshot pointing to the transaction branch head.
    auto snapshot_or = transaction_manager_->CreateSnapshot(*transaction_id);
    if (!snapshot_or.ok())
      return snapshot_or.status();
    return *snapshot_or;
  }

  // Implicit transaction: use RunImplicitTransaction.
  auto commit_result_or = transaction_manager_->RunImplicitTransaction([&](const std::string& txn_id) -> absl::Status {
    // The txn_id IS the branch name — use it directly.
    return write_fn(txn_id);
  });
  if (!commit_result_or.ok())
    return commit_result_or.status();

  auto& commit_result = *commit_result_or;
  if (std::holds_alternative<transaction::TransactionManager::CommitConflict>(commit_result)) {
    auto& conflict = std::get<transaction::TransactionManager::CommitConflict>(commit_result);
    return absl::AbortedError(absl::StrCat("implicit transaction commit conflict on transaction ", conflict.transaction_id));
  }

  auto& success = std::get<transaction::TransactionManager::CommitSuccess>(commit_result);
  return success.snapshot_id;
}

// ── Staging operations ──────────────────────────────────────────────────────

absl::Status ArtifactStore::StageCreate(const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name,
                                        const std::string& payload, const google::protobuf::FileDescriptorSet& descriptor_set) {
  // 1. Write the StoredArtifact envelope.
  const std::string envelope = SerializeEnvelope(version_id, type_name, payload);
  const std::string artifact_path = encoding::ArtifactPath(artifact_id);
  auto status = storage_->PutObject(branch, artifact_path, envelope);
  if (!status.ok())
    return status;

  // 2. Derive and write index entries.
  auto entries_or = index::DeriveIndexEntriesFromPayload(descriptor_set, type_name, payload, artifact_id, options_.index_def_ids_by_key_type);
  if (!entries_or.ok())
    return entries_or.status();

  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return absl::InternalError(absl::StrCat("message '", type_name, "' not found in descriptor set for index derivation"));
  }

  for (const auto& entry : *entries_or) {
    auto index_def = FindIndexDefinition(*descriptor, entry.key_type);
    if (!index_def.has_value()) {
      return absl::InternalError(absl::StrCat("IndexDefinition not found for key_type: ", entry.key_type));
    }
    status = AddIndexRow(storage_, branch, entry, artifact_id, *index_def, *descriptor);
    if (!status.ok())
      return status;
  }

  return absl::OkStatus();
}

absl::Status ArtifactStore::StageUpdate(const std::string& branch, uint64_t artifact_id, uint64_t version_id, const std::string& type_name,
                                        const std::string& payload, const google::protobuf::FileDescriptorSet& descriptor_set) {
  // 1. Read the existing artifact to get old index entries.
  auto existing_or = ReadStoredArtifact(storage_, branch, artifact_id);
  if (!existing_or.ok())
    return existing_or.status();

  // 2. Derive old index entries (for removal).
  // Use the old artifact's version_id to get the correct descriptor_set
  // for deriving old entries, in case the schema changed between versions.
  std::vector<index::DerivedIndexEntry> old_entries;
  google::protobuf::FileDescriptorSet old_descriptor_set = descriptor_set;
  if (!existing_or->payload().empty()) {
    // Try to read the old version's descriptor_set.
    auto old_tvd_or = ReadStoredArtifact(storage_, branch, existing_or->version_id());
    if (old_tvd_or.ok()) {
      TypeVersionDefinition old_tvd;
      if (old_tvd.ParseFromString(old_tvd_or->payload())) {
        old_descriptor_set = old_tvd.descriptor_set();
      }
    }
    auto old_or = index::DeriveIndexEntriesFromPayload(old_descriptor_set, type_name, existing_or->payload(), artifact_id, options_.index_def_ids_by_key_type);
    if (old_or.ok()) {
      old_entries = std::move(*old_or);
    }
    // If derivation fails for old payload, we continue — best effort cleanup.
  }

  // 3. Write the new StoredArtifact envelope.
  const std::string envelope = SerializeEnvelope(version_id, type_name, payload);
  const std::string artifact_path = encoding::ArtifactPath(artifact_id);
  auto status = storage_->PutObject(branch, artifact_path, envelope);
  if (!status.ok())
    return status;

  // 4. Derive new index entries.
  auto new_entries_or = index::DeriveIndexEntriesFromPayload(descriptor_set, type_name, payload, artifact_id, options_.index_def_ids_by_key_type);
  if (!new_entries_or.ok())
    return new_entries_or.status();

  // 5. Build descriptor pool for new-schema index operations.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return absl::InternalError(absl::StrCat("message '", type_name, "' not found in descriptor set for index operations"));
  }

  // 6. Remove old index entries using the old descriptor set.
  // We must use the old descriptor because index definitions (key_type, key
  // fields) may differ between schema versions.
  if (!old_entries.empty()) {
    google::protobuf::DescriptorPool old_pool(google::protobuf::DescriptorPool::generated_pool());
    const auto* old_descriptor = BuildPoolAndFindMessage(old_descriptor_set, type_name, &old_pool);
    // If the old descriptor can't be resolved, skip cleanup (best-effort).
    if (old_descriptor != nullptr) {
      for (const auto& old_entry : old_entries) {
        auto index_def = FindIndexDefinition(*old_descriptor, old_entry.key_type);
        if (!index_def.has_value())
          continue;
        (void)RemoveIndexRow(storage_, branch, old_entry, artifact_id, *index_def, *old_descriptor);
      }
    }
  }

  // 7. Add new index entries.
  for (const auto& new_entry : *new_entries_or) {
    auto index_def = FindIndexDefinition(*descriptor, new_entry.key_type);
    if (!index_def.has_value())
      continue;
    status = AddIndexRow(storage_, branch, new_entry, artifact_id, *index_def, *descriptor);
    if (!status.ok())
      return status;
  }

  return absl::OkStatus();
}

absl::Status ArtifactStore::StageDelete(const std::string& branch, uint64_t artifact_id, const StoredArtifact& existing) {
  // 1. Write tombstone envelope.
  const std::string tombstone = SerializeTombstone(existing.version_id(), existing.type_name());
  const std::string artifact_path = encoding::ArtifactPath(artifact_id);
  auto status = storage_->PutObject(branch, artifact_path, tombstone);
  if (!status.ok())
    return status;

  // 2. Remove all derived index entries.
  if (existing.payload().empty()) {
    return absl::OkStatus(); // already tombstoned, nothing to remove
  }

  // We need the descriptor_set from the TypeVersionDefinition to derive entries.
  // Read the TVD to get the descriptor_set.
  auto tvd_or = ReadStoredArtifact(storage_, branch, existing.version_id());
  if (!tvd_or.ok()) {
    // If we can't read the TVD, we can't derive old entries. Continue with
    // tombstone write — the entries will be stale but won't cause
    // correctness issues (they point to a tombstoned artifact).
    return absl::OkStatus();
  }
  TypeVersionDefinition tvd;
  if (!tvd.ParseFromString(tvd_or->payload())) {
    return absl::OkStatus(); // best-effort
  }

  auto old_entries_or =
      index::DeriveIndexEntriesFromPayload(tvd.descriptor_set(), existing.type_name(), existing.payload(), artifact_id, options_.index_def_ids_by_key_type);
  if (!old_entries_or.ok()) {
    return absl::OkStatus(); // best-effort
  }

  // Build descriptor pool.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(tvd.descriptor_set(), existing.type_name(), &pool);
  if (descriptor == nullptr) {
    return absl::OkStatus(); // best-effort
  }

  for (const auto& entry : *old_entries_or) {
    auto index_def = FindIndexDefinition(*descriptor, entry.key_type);
    if (!index_def.has_value())
      continue;
    (void)RemoveIndexRow(storage_, branch, entry, artifact_id, *index_def, *descriptor);
  }

  return absl::OkStatus();
}

// ── Delete side effects ────────────────────────────────────────────────────

absl::Status ArtifactStore::ApplyCascadeEffect(const std::string& branch, const CascadeDelete& cascade) {
  auto existing_or = ReadStoredArtifact(storage_, branch, cascade.artifact_id);
  if (!existing_or.ok() || existing_or->payload().empty()) {
    return absl::OkStatus();
  }
  return StageDelete(branch, cascade.artifact_id, *existing_or);
}

absl::Status ArtifactStore::ApplySetNullEffect(const std::string& branch, const SetNullUpdate& set_null) {
  // Read the referencing artifact.
  auto ref_art_or = ReadStoredArtifact(storage_, branch, set_null.referencing_artifact_id);
  if (!ref_art_or.ok() || ref_art_or->payload().empty())
    return absl::OkStatus();

  // Read the TVD to get the descriptor set.
  auto tvd_or = ReadStoredArtifact(storage_, branch, ref_art_or->version_id());
  if (!tvd_or.ok())
    return absl::OkStatus();
  TypeVersionDefinition tvd;
  if (!tvd.ParseFromString(tvd_or->payload()))
    return absl::OkStatus();

  // Build descriptor and parse the payload.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* desc = BuildPoolAndFindMessage(tvd.descriptor_set(), ref_art_or->type_name(), &pool);
  if (desc == nullptr)
    return absl::OkStatus();

  google::protobuf::DynamicMessageFactory factory;
  const auto* proto = factory.GetPrototype(desc);
  std::unique_ptr<google::protobuf::Message> msg(proto->New());
  if (!msg->ParseFromString(ref_art_or->payload()))
    return absl::OkStatus();

  // Find and clear the reference field.
  const auto* field = desc->FindFieldByName(set_null.field_name);
  if (field == nullptr)
    return absl::OkStatus();

  const auto* reflection = msg->GetReflection();
  if (field->is_repeated()) {
    const int count = reflection->FieldSize(*msg, field);
    std::unique_ptr<google::protobuf::Message> new_msg(proto->New());
    new_msg->CopyFrom(*msg);
    const auto* new_reflection = new_msg->GetReflection();
    new_reflection->ClearField(new_msg.get(), field);
    for (int j = 0; j < count; ++j) {
      const uint64_t val = reflection->GetRepeatedUInt64(*msg, field, j);
      if (val != set_null.removed_reference_id) {
        new_reflection->AddUInt64(new_msg.get(), field, val);
      }
    }
    msg = std::move(new_msg);
  } else {
    reflection->ClearField(msg.get(), field);
  }

  // Write back the modified artifact.
  const std::string new_payload = msg->SerializeAsString();
  return StageUpdate(branch, set_null.referencing_artifact_id, ref_art_or->version_id(), ref_art_or->type_name(), new_payload, tvd.descriptor_set());
}

// ── CRUD operations ─────────────────────────────────────────────────────────

absl::StatusOr<CreateResult> ArtifactStore::CreateArtifact(uint64_t version_id, const std::string& payload, const std::optional<std::string>& transaction_id) {
  // Allocate ID first (before validation, as per PRD flow).
  const uint64_t artifact_id = id_allocator_->AllocateId();

  // Resolve the write branch for validation reads.
  auto branch_or = ResolveWriteBranch(transaction_id);
  if (!branch_or.ok())
    return branch_or.status();
  const std::string& read_ref = *branch_or;

  // Run validation pipeline (phases 1-5).
  ValidationContext vctx{storage_, read_ref, options_.bypass_mutation_check};
  auto val_result_or = ValidateCreateOrUpdate(WriteOperation::kCreate, version_id, payload, vctx, std::nullopt, options_.index_def_ids_by_key_type);
  if (!val_result_or.ok())
    return val_result_or.status();

  auto& val_result = *val_result_or;

  // If phases 1-4 short-circuited (no resolved_type), return immediately.
  if (!val_result.resolved_type.has_value()) {
    return MakeWriteError(val_result.violations);
  }
  const ResolvedType& resolved = *val_result.resolved_type;

  // Run referential integrity validation (phase 6) — runs independently
  // of phase 5 (NaN/varint). Both phases' violations are collected.
  if (!options_.bypass_referential_integrity) {
    auto ref_violations_or = RunRefIntegrityValidation(storage_, read_ref, resolved, payload);
    if (!ref_violations_or.ok())
      return ref_violations_or.status();
    val_result.violations.insert(val_result.violations.end(), ref_violations_or->begin(), ref_violations_or->end());
  }

  if (!val_result.violations.empty())
    return MakeWriteError(val_result.violations);

  // Stage the write.
  auto snapshot_or = ExecuteWrite(transaction_id, [&](const std::string& branch) -> absl::Status {
    return StageCreate(branch, artifact_id, version_id, resolved.type_name, payload, resolved.descriptor_set);
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return CreateResult{artifact_id, *snapshot_or};
}

absl::StatusOr<WriteResult> ArtifactStore::UpdateArtifact(uint64_t artifact_id, uint64_t version_id, const std::string& payload,
                                                          const std::optional<std::string>& transaction_id) {
  auto branch_or = ResolveWriteBranch(transaction_id);
  if (!branch_or.ok())
    return branch_or.status();
  const std::string& read_ref = *branch_or;

  // Check existing artifact exists and is not tombstoned.
  auto existing_or = ReadStoredArtifact(storage_, read_ref, artifact_id);
  if (!existing_or.ok()) {
    if (absl::IsNotFound(existing_or.status())) {
      return MakeNotFoundError(artifact_id, false);
    }
    return existing_or.status();
  }
  if (existing_or->payload().empty()) {
    return MakeNotFoundError(artifact_id, true);
  }

  // Run validation pipeline (phases 1-5).
  ValidationContext vctx{storage_, read_ref, options_.bypass_mutation_check};
  auto val_result_or = ValidateCreateOrUpdate(WriteOperation::kUpdate, version_id, payload, vctx, artifact_id, options_.index_def_ids_by_key_type);
  if (!val_result_or.ok())
    return val_result_or.status();

  auto& val_result = *val_result_or;

  // If phases 1-4 short-circuited (no resolved_type), return immediately.
  if (!val_result.resolved_type.has_value()) {
    return MakeWriteError(val_result.violations);
  }
  const ResolvedType& resolved = *val_result.resolved_type;

  // Referential integrity validation (phase 6) — independent of phase 5.
  if (!options_.bypass_referential_integrity) {
    auto ref_violations_or = RunRefIntegrityValidation(storage_, read_ref, resolved, payload);
    if (!ref_violations_or.ok())
      return ref_violations_or.status();
    val_result.violations.insert(val_result.violations.end(), ref_violations_or->begin(), ref_violations_or->end());
  }

  if (!val_result.violations.empty())
    return MakeWriteError(val_result.violations);

  auto snapshot_or = ExecuteWrite(transaction_id, [&](const std::string& branch) -> absl::Status {
    return StageUpdate(branch, artifact_id, version_id, resolved.type_name, payload, resolved.descriptor_set);
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return WriteResult{*snapshot_or};
}

absl::StatusOr<WriteResult> ArtifactStore::DeleteArtifact(uint64_t artifact_id, const std::optional<std::string>& transaction_id) {
  auto branch_or = ResolveWriteBranch(transaction_id);
  if (!branch_or.ok())
    return branch_or.status();
  const std::string& read_ref = *branch_or;

  // Read existing artifact.
  auto existing_or = ReadStoredArtifact(storage_, read_ref, artifact_id);
  if (!existing_or.ok()) {
    if (absl::IsNotFound(existing_or.status())) {
      return MakeNotFoundError(artifact_id, false);
    }
    return existing_or.status();
  }
  const StoredArtifact& existing = *existing_or;
  if (existing.payload().empty()) {
    return MakeNotFoundError(artifact_id, true);
  }

  // Validate mutation restrictions.
  ValidationContext vctx{storage_, read_ref, options_.bypass_mutation_check};
  auto violations_or = ValidateDelete(artifact_id, vctx);
  if (!violations_or.ok())
    return violations_or.status();
  if (!violations_or->empty())
    return MakeWriteError(*violations_or);

  // Referential integrity enforcement.
  RefIntegrityContext ri_ctx{storage_, read_ref};
  std::set<uint64_t> scheduled_deletes{artifact_id};
  auto enforce_or = EnforceDeleteIntegrity(artifact_id, existing.type_name(), ri_ctx, scheduled_deletes, options_.index_def_ids_by_key_type);
  if (!enforce_or.ok())
    return enforce_or.status();

  if (!enforce_or->violations.empty()) {
    return MakeWriteError(enforce_or->violations);
  }

  // Collect all side effects for execution within the same write.
  auto side_effects = std::move(enforce_or->side_effects);

  auto snapshot_or = ExecuteWrite(transaction_id, [&](const std::string& branch) -> absl::Status {
    auto status = StageDelete(branch, artifact_id, existing);
    if (!status.ok())
      return status;

    for (const auto& effect : side_effects) {
      if (std::holds_alternative<CascadeDelete>(effect)) {
        status = ApplyCascadeEffect(branch, std::get<CascadeDelete>(effect));
      } else if (std::holds_alternative<SetNullUpdate>(effect)) {
        status = ApplySetNullEffect(branch, std::get<SetNullUpdate>(effect));
      }
      if (!status.ok())
        return status;
    }
    return absl::OkStatus();
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return WriteResult{*snapshot_or};
}

} // namespace artifact_system::artifact

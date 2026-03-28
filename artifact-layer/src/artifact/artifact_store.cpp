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

#include "artifact/referential_integrity.h"
#include "artifact/validation.h"
#include "artifact_internal.pb.h"
#include "artifact_service.pb.h"
#include "artifact_types.pb.h"
#include "encoding/artifact_path.h"
#include "index/index_derivation.h"
#include "index/index_object.h"
#include "index/index_schema_generator.h"

namespace artifact_system::artifact {
namespace {

// Build a DescriptorPool and find a message by full name.
const google::protobuf::Descriptor* BuildPoolAndFindMessage(const google::protobuf::FileDescriptorSet& descriptor_set, const std::string& message_full_name,
                                                            google::protobuf::DescriptorPool* pool) {
  std::vector<bool> built(descriptor_set.file_size(), false);
  int built_count = 0;
  bool made_progress = true;
  while (built_count < descriptor_set.file_size() && made_progress) {
    made_progress = false;
    for (int i = 0; i < descriptor_set.file_size(); ++i) {
      if (built[static_cast<size_t>(i)])
        continue;
      const auto& file = descriptor_set.file(i);
      if (pool->FindFileByName(file.name()) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
        continue;
      }
      if (pool->BuildFile(file) != nullptr) {
        built[static_cast<size_t>(i)] = true;
        ++built_count;
        made_progress = true;
      }
    }
  }
  return pool->FindMessageTypeByName(message_full_name);
}

// Read and parse a StoredArtifact from storage.
absl::StatusOr<StoredArtifact> ReadStoredArtifact(StorageInterface* storage, const std::string& ref, uint64_t artifact_id) {
  const std::string path = encoding::ArtifactPath(artifact_id);
  auto data_or = storage->GetObject(ref, path);
  if (!data_or.ok())
    return data_or.status();
  StoredArtifact stored;
  if (!stored.ParseFromString(*data_or)) {
    return absl::InternalError(absl::StrCat("failed to parse StoredArtifact at ", path));
  }
  return stored;
}

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
    auto meta_or = transaction_manager_->GetSnapshotMetadata(context.snapshot_id());
    if (!meta_or.ok())
      return meta_or.status();
    return meta_or->commit_id;
  }
  if (context.has_transaction_id()) {
    auto meta_or = transaction_manager_->GetTransactionMetadata(context.transaction_id());
    if (!meta_or.ok())
      return meta_or.status();
    return meta_or->branch_name;
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

absl::StatusOr<std::string> ArtifactStore::ResolveWriteBranch(std::optional<uint64_t> transaction_id) {
  if (transaction_id.has_value()) {
    auto meta_or = transaction_manager_->GetTransactionMetadata(*transaction_id);
    if (!meta_or.ok())
      return meta_or.status();
    return meta_or->branch_name;
  }
  return std::string(storage_->GetCanonicalBranch());
}

absl::StatusOr<uint64_t> ArtifactStore::ExecuteWrite(std::optional<uint64_t> transaction_id, const WriteFn& write_fn) {
  if (transaction_id.has_value()) {
    // Explicit transaction: use WriteExecutor against the transaction branch.
    auto meta_or = transaction_manager_->GetTransactionMetadata(*transaction_id);
    if (!meta_or.ok())
      return meta_or.status();

    transaction::WriteExecutor executor(storage_);
    auto result_or = executor.ExecuteWrite(meta_or->branch_name, [&write_fn](const std::string& child_branch) { return write_fn(child_branch); });
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
  uint64_t snapshot_id = 0;
  auto commit_result_or = transaction_manager_->RunImplicitTransaction([&](uint64_t txn_id) -> absl::Status {
    auto txn_meta_or = transaction_manager_->GetTransactionMetadata(txn_id);
    if (!txn_meta_or.ok())
      return txn_meta_or.status();
    return write_fn(txn_meta_or->branch_name);
  });
  if (!commit_result_or.ok())
    return commit_result_or.status();

  auto& commit_result = *commit_result_or;
  if (std::holds_alternative<transaction::TransactionManager::CommitConflict>(commit_result)) {
    auto& conflict = std::get<transaction::TransactionManager::CommitConflict>(commit_result);
    return absl::AbortedError(absl::StrCat("implicit transaction commit conflict on transaction ", conflict.transaction_id));
  }

  auto& success = std::get<transaction::TransactionManager::CommitSuccess>(commit_result);
  if (success.snapshot_id.has_value()) {
    return *success.snapshot_id;
  }
  // Create a snapshot from the commit.
  auto snap_or = transaction_manager_->CreateSnapshot();
  if (!snap_or.ok())
    return snap_or.status();
  return *snap_or;
}

// ── Index operations ────────────────────────────────────────────────────────

absl::Status ArtifactStore::WriteIndexEntries(const std::string& branch, const std::vector<index::DerivedIndexEntry>& entries, uint64_t artifact_id, bool add) {
  for (const auto& entry : entries) {
    const std::string path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

    // Read existing index object at this path (may not exist).
    index::IndexObject object;
    object.serialized_key = std::string(entry.encoded_key.begin(), entry.encoded_key.end());

    // We need the IndexDefinition and parent descriptor to use
    // Serialize/DeserializeIndexObject. For the MVP, we build these from
    // the key_type. We need to find the IndexDefinition proto for this
    // key_type. Since we don't have it readily available, we construct a
    // minimal one.
    //
    // Actually, we need to know the parent message descriptor for
    // GenerateIndexSchema. For now, we try to read/modify the index object
    // using direct serialization matching what the index system uses.

    auto existing_or = storage_->GetObject(branch, path);
    if (existing_or.ok()) {
      // There's an existing index object — we need to deserialize, modify,
      // and reserialize. However, we need the schema to do that. For a
      // simpler approach in the CRUD layer, we'll re-derive the full index
      // object from scratch during staging operations (StageCreate,
      // StageUpdate, StageDelete) rather than doing incremental updates here.
      //
      // This is a placeholder — the actual index read/modify/write is done
      // in the staging methods below using the full context.
    }

    // For add: read existing, add row, write back.
    // For remove: read existing, remove row, write back (or tombstone if empty).
    // This method is called with the full schema context from the staging methods.
    //
    // Since we need the schema for proper serialization, we pass through to
    // the caller-managed index update logic. This WriteIndexEntries is a
    // convenience wrapper — the real work happens in StageCreate/Update/Delete.
  }
  return absl::OkStatus();
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

  // Build a descriptor pool for index schema generation.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return absl::InternalError(absl::StrCat("message '", type_name, "' not found in descriptor set for index derivation"));
  }

  for (const auto& entry : *entries_or) {
    const std::string index_path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

    // Find the IndexDefinition for this entry's key_type.
    IndexDefinition index_def;
    const auto& options = descriptor->options();
    bool found = false;
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& def = options.GetExtension(artifact_system::indexes, i);
      if (def.key_type() == entry.key_type) {
        index_def = def;
        found = true;
        break;
      }
    }
    if (!found) {
      return absl::InternalError(absl::StrCat("IndexDefinition not found for key_type: ", entry.key_type));
    }

    auto schema_or = index::GenerateIndexSchema(index_def, *descriptor);
    if (!schema_or.ok())
      return schema_or.status();

    // Read existing index object or start fresh.
    index::IndexObject index_obj;
    index_obj.serialized_key = std::string(entry.encoded_key.begin(), entry.encoded_key.end());

    auto existing_or = storage_->GetObject(branch, index_path);
    if (existing_or.ok()) {
      auto deser_or = index::DeserializeIndexObject(*schema_or, index_def, *existing_or);
      if (deser_or.ok()) {
        index_obj = std::move(*deser_or);
      }
    }

    // Add the new row.
    index::IndexRow row;
    row.artifact_id = artifact_id;
    row.order_values = entry.order_values;
    index_obj.rows.push_back(std::move(row));

    // Serialize and write back.
    auto ser_or = index::SerializeIndexObject(*schema_or, index_def, index_obj);
    if (!ser_or.ok())
      return ser_or.status();
    status = storage_->PutObject(branch, index_path, *ser_or);
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
  std::vector<index::DerivedIndexEntry> old_entries;
  if (!existing_or->payload().empty()) {
    auto old_or = index::DeriveIndexEntriesFromPayload(descriptor_set, type_name, existing_or->payload(), artifact_id, options_.index_def_ids_by_key_type);
    if (old_or.ok()) {
      old_entries = std::move(*old_or);
    }
    // If derivation fails for old payload, we continue — the old entries
    // may have been written with a different schema version.
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

  // 5. Build descriptor pool for index operations.
  google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
  const auto* descriptor = BuildPoolAndFindMessage(descriptor_set, type_name, &pool);
  if (descriptor == nullptr) {
    return absl::InternalError(absl::StrCat("message '", type_name, "' not found in descriptor set for index operations"));
  }

  // 6. Remove old index entries.
  for (const auto& old_entry : old_entries) {
    const std::string index_path = encoding::IndexPath(old_entry.index_def_id, old_entry.encoded_key);

    IndexDefinition index_def;
    const auto& options = descriptor->options();
    bool found = false;
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& def = options.GetExtension(artifact_system::indexes, i);
      if (def.key_type() == old_entry.key_type) {
        index_def = def;
        found = true;
        break;
      }
    }
    if (!found)
      continue;

    auto schema_or = index::GenerateIndexSchema(index_def, *descriptor);
    if (!schema_or.ok())
      continue;

    auto existing_idx_or = storage_->GetObject(branch, index_path);
    if (!existing_idx_or.ok())
      continue;

    auto deser_or = index::DeserializeIndexObject(*schema_or, index_def, *existing_idx_or);
    if (!deser_or.ok())
      continue;

    auto& idx_obj = *deser_or;
    // Remove the row with this artifact_id.
    std::erase_if(idx_obj.rows, [artifact_id](const index::IndexRow& row) { return row.artifact_id == artifact_id; });

    if (idx_obj.rows.empty()) {
      // Tombstone: write empty index object (zero rows).
      auto ser_or = index::SerializeIndexObject(*schema_or, index_def, idx_obj);
      if (ser_or.ok()) {
        (void)storage_->PutObject(branch, index_path, *ser_or);
      }
    } else {
      auto ser_or = index::SerializeIndexObject(*schema_or, index_def, idx_obj);
      if (ser_or.ok()) {
        (void)storage_->PutObject(branch, index_path, *ser_or);
      }
    }
  }

  // 7. Add new index entries.
  for (const auto& new_entry : *new_entries_or) {
    const std::string index_path = encoding::IndexPath(new_entry.index_def_id, new_entry.encoded_key);

    IndexDefinition index_def;
    const auto& options = descriptor->options();
    bool found = false;
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& def = options.GetExtension(artifact_system::indexes, i);
      if (def.key_type() == new_entry.key_type) {
        index_def = def;
        found = true;
        break;
      }
    }
    if (!found)
      continue;

    auto schema_or = index::GenerateIndexSchema(index_def, *descriptor);
    if (!schema_or.ok())
      return schema_or.status();

    index::IndexObject index_obj;
    index_obj.serialized_key = std::string(new_entry.encoded_key.begin(), new_entry.encoded_key.end());

    auto existing_idx_or = storage_->GetObject(branch, index_path);
    if (existing_idx_or.ok()) {
      auto deser_or = index::DeserializeIndexObject(*schema_or, index_def, *existing_idx_or);
      if (deser_or.ok()) {
        index_obj = std::move(*deser_or);
      }
    }

    index::IndexRow row;
    row.artifact_id = artifact_id;
    row.order_values = new_entry.order_values;
    index_obj.rows.push_back(std::move(row));

    auto ser_or = index::SerializeIndexObject(*schema_or, index_def, index_obj);
    if (!ser_or.ok())
      return ser_or.status();
    status = storage_->PutObject(branch, index_path, *ser_or);
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
    const std::string index_path = encoding::IndexPath(entry.index_def_id, entry.encoded_key);

    IndexDefinition index_def;
    const auto& options = descriptor->options();
    bool found = false;
    for (int i = 0; i < options.ExtensionSize(artifact_system::indexes); ++i) {
      const auto& def = options.GetExtension(artifact_system::indexes, i);
      if (def.key_type() == entry.key_type) {
        index_def = def;
        found = true;
        break;
      }
    }
    if (!found)
      continue;

    auto schema_or = index::GenerateIndexSchema(index_def, *descriptor);
    if (!schema_or.ok())
      continue;

    auto existing_idx_or = storage_->GetObject(branch, index_path);
    if (!existing_idx_or.ok())
      continue;

    auto deser_or = index::DeserializeIndexObject(*schema_or, index_def, *existing_idx_or);
    if (!deser_or.ok())
      continue;

    auto& idx_obj = *deser_or;
    std::erase_if(idx_obj.rows, [artifact_id](const index::IndexRow& row) { return row.artifact_id == artifact_id; });

    // Write back (even if empty — tombstoned index).
    auto ser_or = index::SerializeIndexObject(*schema_or, index_def, idx_obj);
    if (ser_or.ok()) {
      (void)storage_->PutObject(branch, index_path, *ser_or);
    }
  }

  return absl::OkStatus();
}

// ── CRUD operations ─────────────────────────────────────────────────────────

absl::StatusOr<CreateResult> ArtifactStore::CreateArtifact(uint64_t version_id, const std::string& payload, std::optional<uint64_t> transaction_id) {
  // Allocate ID first (before validation, as per PRD flow).
  const uint64_t artifact_id = id_allocator_->AllocateId();

  // Resolve the write branch for validation reads.
  auto branch_or = ResolveWriteBranch(transaction_id);
  if (!branch_or.ok())
    return branch_or.status();
  const std::string& read_ref = *branch_or;

  // Run validation pipeline.
  ValidationContext vctx{storage_, read_ref, options_.bypass_mutation_check};
  auto violations_or = ValidateCreateOrUpdate(WriteOperation::kCreate, version_id, payload, vctx, std::nullopt, options_.index_def_ids_by_key_type);
  if (!violations_or.ok())
    return violations_or.status();

  // Resolve type for referential integrity and staging.
  auto resolved_or = ResolveVersionId(version_id, vctx);
  if (!resolved_or.ok()) {
    // If resolve fails but ValidateCreateOrUpdate already caught it,
    // violations_or should be non-empty.
    if (!violations_or->empty())
      return MakeWriteError(*violations_or);
    return resolved_or.status();
  }
  const ResolvedType& resolved = *resolved_or;

  // Run referential integrity validation (phase 6).
  if (violations_or->empty()) {
    // Parse the payload to a dynamic message for reference checking.
    google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
    const auto* descriptor = BuildPoolAndFindMessage(resolved.descriptor_set, resolved.type_name, &pool);
    if (descriptor != nullptr) {
      google::protobuf::DynamicMessageFactory factory;
      const auto* prototype = factory.GetPrototype(descriptor);
      std::unique_ptr<google::protobuf::Message> msg(prototype->New());
      if (msg->ParseFromString(payload)) {
        RefIntegrityContext ri_ctx{storage_, read_ref};
        auto ref_violations_or = ValidateReferences(*msg, *descriptor, ri_ctx);
        if (ref_violations_or.ok()) {
          violations_or->insert(violations_or->end(), ref_violations_or->begin(), ref_violations_or->end());
        }
      }
    }
  }

  if (!violations_or->empty())
    return MakeWriteError(*violations_or);

  // Stage the write.
  auto snapshot_or = ExecuteWrite(transaction_id, [&](const std::string& branch) -> absl::Status {
    return StageCreate(branch, artifact_id, version_id, resolved.type_name, payload, resolved.descriptor_set);
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return CreateResult{artifact_id, *snapshot_or};
}

absl::StatusOr<WriteResult> ArtifactStore::UpdateArtifact(uint64_t artifact_id, uint64_t version_id, const std::string& payload,
                                                          std::optional<uint64_t> transaction_id) {
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

  // Run validation pipeline.
  ValidationContext vctx{storage_, read_ref, options_.bypass_mutation_check};
  auto violations_or = ValidateCreateOrUpdate(WriteOperation::kUpdate, version_id, payload, vctx, artifact_id, options_.index_def_ids_by_key_type);
  if (!violations_or.ok())
    return violations_or.status();

  // Resolve type for referential integrity and staging.
  auto resolved_or = ResolveVersionId(version_id, vctx);
  if (!resolved_or.ok()) {
    if (!violations_or->empty())
      return MakeWriteError(*violations_or);
    return resolved_or.status();
  }
  const ResolvedType& resolved = *resolved_or;

  // Referential integrity validation.
  if (violations_or->empty()) {
    google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
    const auto* descriptor = BuildPoolAndFindMessage(resolved.descriptor_set, resolved.type_name, &pool);
    if (descriptor != nullptr) {
      google::protobuf::DynamicMessageFactory factory;
      const auto* prototype = factory.GetPrototype(descriptor);
      std::unique_ptr<google::protobuf::Message> msg(prototype->New());
      if (msg->ParseFromString(payload)) {
        RefIntegrityContext ri_ctx{storage_, read_ref};
        auto ref_violations_or = ValidateReferences(*msg, *descriptor, ri_ctx);
        if (ref_violations_or.ok()) {
          violations_or->insert(violations_or->end(), ref_violations_or->begin(), ref_violations_or->end());
        }
      }
    }
  }

  if (!violations_or->empty())
    return MakeWriteError(*violations_or);

  auto snapshot_or = ExecuteWrite(transaction_id, [&](const std::string& branch) -> absl::Status {
    return StageUpdate(branch, artifact_id, version_id, resolved.type_name, payload, resolved.descriptor_set);
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return WriteResult{*snapshot_or};
}

absl::StatusOr<WriteResult> ArtifactStore::DeleteArtifact(uint64_t artifact_id, std::optional<uint64_t> transaction_id) {
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
    // Stage the primary delete.
    auto status = StageDelete(branch, artifact_id, existing);
    if (!status.ok())
      return status;

    // Process side effects (CASCADE deletes, SET_NULL updates).
    for (const auto& effect : side_effects) {
      if (std::holds_alternative<CascadeDelete>(effect)) {
        const auto& cascade = std::get<CascadeDelete>(effect);
        auto cascade_existing_or = ReadStoredArtifact(storage_, branch, cascade.artifact_id);
        if (cascade_existing_or.ok() && !cascade_existing_or->payload().empty()) {
          status = StageDelete(branch, cascade.artifact_id, *cascade_existing_or);
          if (!status.ok())
            return status;
        }
      } else if (std::holds_alternative<SetNullUpdate>(effect)) {
        const auto& set_null = std::get<SetNullUpdate>(effect);
        // Read the referencing artifact, clear the reference field,
        // and write it back.
        auto ref_art_or = ReadStoredArtifact(storage_, branch, set_null.referencing_artifact_id);
        if (!ref_art_or.ok() || ref_art_or->payload().empty())
          continue;

        // Read the TVD to get the descriptor set.
        auto tvd_or = ReadStoredArtifact(storage_, branch, ref_art_or->version_id());
        if (!tvd_or.ok())
          continue;
        TypeVersionDefinition tvd;
        if (!tvd.ParseFromString(tvd_or->payload()))
          continue;

        // Parse and modify the referencing artifact's payload.
        google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
        const auto* desc = BuildPoolAndFindMessage(tvd.descriptor_set(), ref_art_or->type_name(), &pool);
        if (desc == nullptr)
          continue;

        google::protobuf::DynamicMessageFactory factory;
        const auto* proto = factory.GetPrototype(desc);
        std::unique_ptr<google::protobuf::Message> msg(proto->New());
        if (!msg->ParseFromString(ref_art_or->payload()))
          continue;

        // Find and clear the reference field.
        const auto* field = desc->FindFieldByName(set_null.field_name);
        if (field == nullptr)
          continue;

        const auto* reflection = msg->GetReflection();
        if (field->is_repeated()) {
          // Remove the specific value from the repeated field.
          const int count = reflection->FieldSize(*msg, field);
          std::unique_ptr<google::protobuf::Message> new_msg(proto->New());
          new_msg->CopyFrom(*msg);
          const auto* new_reflection = new_msg->GetReflection();
          // Clear and re-add all except the removed reference.
          new_reflection->ClearField(new_msg.get(), field);
          for (int j = 0; j < count; ++j) {
            const uint64_t val = reflection->GetRepeatedUInt64(*msg, field, j);
            if (val != set_null.removed_reference_id) {
              new_reflection->AddUInt64(new_msg.get(), field, val);
            }
          }
          msg = std::move(new_msg);
        } else {
          // Clear the optional field.
          reflection->ClearField(msg.get(), field);
        }

        // Write back the modified artifact.
        const std::string new_payload = msg->SerializeAsString();
        status = StageUpdate(branch, set_null.referencing_artifact_id, ref_art_or->version_id(), ref_art_or->type_name(), new_payload, tvd.descriptor_set());
        if (!status.ok())
          return status;
      }
    }
    return absl::OkStatus();
  });
  if (!snapshot_or.ok())
    return snapshot_or.status();

  return WriteResult{*snapshot_or};
}

} // namespace artifact_system::artifact

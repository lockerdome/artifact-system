#include "service/index_service_impl.h"

#include <span>
#include <vector>

#include "absl/strings/str_cat.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/dynamic_message.h"

#include "artifact/proto_utils.h"
#include "encoding/artifact_path.h"
#include "encoding/index_key_encoder.h"
#include "service/grpc_error_util.h"

namespace artifact_system::service {

namespace {

// Build an absl::Status with a FetchIndexError detail payload.
absl::Status MakeFetchIndexError(absl::StatusCode code, const std::string& message, FetchIndexError::Category category, const std::string& key_type) {
  FetchIndexError error;
  error.set_category(category);
  error.set_description(message);
  error.set_key_type(key_type);
  return MakeStatusWithDetail(code, message, error);
}

// Build an absl::Status with a SnapshotTransactionError detail payload.
absl::Status MakeSnapshotTxnError(const std::string& message, SnapshotTransactionError::Category category, const std::string& id) {
  SnapshotTransactionError error;
  error.set_category(category);
  error.set_description(message);
  error.set_id(id);
  return MakeStatusWithDetail(absl::StatusCode::kNotFound, message, error);
}

} // namespace

IndexServiceImpl::IndexServiceImpl(StorageInterface* storage, transaction::TransactionManager* txn_manager, registry::TypeRegistry* registry)
    : storage_(storage), txn_manager_(txn_manager), registry_(registry) {
}

absl::StatusOr<std::string> IndexServiceImpl::ResolveReadRef(const ReadContext& read_context) {
  if (read_context.has_snapshot_id()) {
    auto meta = txn_manager_->GetSnapshotMetadata(read_context.snapshot_id());
    if (!meta.ok()) {
      return MakeSnapshotTxnError(std::string(meta.status().message()), SnapshotTransactionError::SNAPSHOT_NOT_FOUND, read_context.snapshot_id());
    }
    return read_context.snapshot_id();
  }
  if (read_context.has_transaction_id()) {
    auto meta = txn_manager_->GetTransactionMetadata(read_context.transaction_id());
    if (!meta.ok()) {
      return MakeSnapshotTxnError(std::string(meta.status().message()), SnapshotTransactionError::TRANSACTION_NOT_FOUND, read_context.transaction_id());
    }
    return read_context.transaction_id();
  }
  return std::string(storage_->GetCanonicalBranch());
}

grpc::Status IndexServiceImpl::FetchIndex(grpc::ServerContext* /*context*/, const FetchIndexRequest* request, FetchIndexResponse* response) {
  const std::string& key_type = request->key_type();

  // Step 1: Get the index schema from the registry.
  auto schema_info_or = registry_->GetIndexSchema(key_type);
  if (!schema_info_or.ok()) {
    if (absl::IsNotFound(schema_info_or.status())) {
      return AbslToGrpcStatus(MakeFetchIndexError(absl::StatusCode::kNotFound, absl::StrCat("index not found for key_type: ", key_type),
                                                  FetchIndexError::INDEX_NOT_FOUND, key_type));
    }
    return AbslToGrpcStatus(schema_info_or.status());
  }
  const registry::IndexSchemaInfo& schema_info = *schema_info_or;

  // Step 2: Resolve ReadContext to a storage ref.
  auto ref_or = ResolveReadRef(request->context());
  if (!ref_or.ok()) {
    return AbslToGrpcStatus(ref_or.status());
  }
  const std::string& ref = *ref_or;

  // Step 3: Build a DescriptorPool from the index descriptor set and find the
  //         key message descriptor.
  google::protobuf::DescriptorPool pool;
  const google::protobuf::Descriptor* key_desc = artifact::BuildPoolAndFindMessage(schema_info.index_descriptor_set, schema_info.key_message_name, &pool);
  if (key_desc == nullptr) {
    return AbslToGrpcStatus(MakeFetchIndexError(absl::StatusCode::kInternal, absl::StrCat("failed to build key descriptor for key_type: ", key_type),
                                                FetchIndexError::KEY_PARSE_FAILURE, key_type));
  }

  // Step 4: Parse the request key bytes using the key descriptor.
  google::protobuf::DynamicMessageFactory factory;
  const google::protobuf::Message* key_prototype = factory.GetPrototype(key_desc);
  std::unique_ptr<google::protobuf::Message> key_msg(key_prototype->New());
  if (!key_msg->ParseFromString(request->key())) {
    return AbslToGrpcStatus(MakeFetchIndexError(absl::StatusCode::kInvalidArgument, absl::StrCat("failed to parse key bytes for key_type: ", key_type),
                                                FetchIndexError::KEY_PARSE_FAILURE, key_type));
  }

  // Step 5: Validate completeness — all key fields must be set.
  const google::protobuf::Reflection* refl = key_msg->GetReflection();
  for (int i = 0; i < key_desc->field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = key_desc->field(i);
    if (!refl->HasField(*key_msg, field)) {
      return AbslToGrpcStatus(MakeFetchIndexError(absl::StatusCode::kInvalidArgument,
                                                  absl::StrCat("missing key field '", field->name(), "' for key_type: ", key_type),
                                                  FetchIndexError::INCOMPLETE_KEY, key_type));
    }
  }

  // Step 6: Encode the key fields to a deterministic binary representation.
  auto encoded_key_or = encoding::EncodeKey(*key_desc, *key_msg, schema_info.key_fields);
  if (!encoded_key_or.ok()) {
    return AbslToGrpcStatus(MakeFetchIndexError(absl::StatusCode::kInvalidArgument,
                                                absl::StrCat("failed to encode key for key_type: ", key_type, ": ", encoded_key_or.status().message()),
                                                FetchIndexError::KEY_PARSE_FAILURE, key_type));
  }

  // Step 7: Build the storage path and read the index object.
  std::span<const uint8_t> key_span(*encoded_key_or);
  const std::string path = encoding::IndexPath(schema_info.index_definition_id, key_span);

  auto data_or = storage_->GetObject(ref, path);
  if (!data_or.ok()) {
    if (absl::IsNotFound(data_or.status())) {
      // Index entry does not exist — return an empty index message.
      // The pool already has files built from step 3; FindMessage works here.
      const google::protobuf::Descriptor* index_desc = pool.FindMessageTypeByName(schema_info.index_message_name);
      if (index_desc != nullptr) {
        const google::protobuf::Message* index_prototype = factory.GetPrototype(index_desc);
        std::unique_ptr<google::protobuf::Message> empty_msg(index_prototype->New());
        response->set_index_payload(empty_msg->SerializeAsString());
      }
      response->set_index_message_name(schema_info.index_message_name);
      return grpc::Status::OK;
    }
    return AbslToGrpcStatus(data_or.status());
  }

  // Step 8: Return the raw index bytes and message name.
  response->set_index_payload(*data_or);
  response->set_index_message_name(schema_info.index_message_name);
  return grpc::Status::OK;
}

} // namespace artifact_system::service

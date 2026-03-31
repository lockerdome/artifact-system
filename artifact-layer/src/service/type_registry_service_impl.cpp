#include "service/type_registry_service_impl.h"

#include <stdexcept>

#include "service/grpc_error_util.h"

namespace artifact_system::service {

TypeRegistryServiceImpl::TypeRegistryServiceImpl(registry::TypeRegistry* registry, artifact::ArtifactStore* artifact_store)
    : registry_(registry), artifact_store_(artifact_store) {
}

grpc::Status TypeRegistryServiceImpl::RegisterTypeVersion(grpc::ServerContext* /*context*/, const RegisterTypeVersionRequest* request,
                                                          RegisterTypeVersionResponse* response) {
  std::optional<bool> deny_create = request->has_deny_create() ? std::optional(request->deny_create()) : std::nullopt;
  std::optional<bool> deny_update = request->has_deny_update() ? std::optional(request->deny_update()) : std::nullopt;
  std::optional<bool> deny_delete = request->has_deny_delete() ? std::optional(request->deny_delete()) : std::nullopt;

  absl::StatusOr<registry::RegisterResult> result;
  try {
    result = registry_->RegisterTypeVersion(request->type_name(), request->proto_source(), deny_create, deny_update, deny_delete);
  } catch (const std::runtime_error& e) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, e.what());
  }

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  // Propagate new index IDs to the artifact store so subsequent CRUD operations
  // can derive indexes for the newly registered type.
  if (artifact_store_) {
    artifact_store_->UpdateIndexDefIds(registry_->index_def_ids_by_key_type());
  }

  response->set_version_id(result->version_id);
  return grpc::Status::OK;
}

grpc::Status TypeRegistryServiceImpl::GetTypeVersion(grpc::ServerContext* /*context*/, const GetTypeVersionRequest* request, GetTypeVersionResponse* response) {
  auto result = registry_->GetTypeVersion(request->version_id());
  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  response->set_version_id(result->version_id);
  response->set_type_id(result->type_id);
  *response->mutable_descriptor_set() = result->descriptor_set;
  response->set_proto_source(result->proto_source);

  if (result->previous_version_id.has_value()) {
    response->set_previous_version_id(*result->previous_version_id);
  }
  if (result->next_version_id.has_value()) {
    response->set_next_version_id(*result->next_version_id);
  }

  return grpc::Status::OK;
}

grpc::Status TypeRegistryServiceImpl::ListTypeVersions(grpc::ServerContext* /*context*/, const ListTypeVersionsRequest* request,
                                                       ListTypeVersionsResponse* response) {
  auto result = registry_->ListTypeVersions(request->type_name());
  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  for (uint64_t version_id : *result) {
    response->add_version_ids(version_id);
  }

  return grpc::Status::OK;
}

grpc::Status TypeRegistryServiceImpl::GetIndexSchema(grpc::ServerContext* /*context*/, const GetIndexSchemaRequest* request, GetIndexSchemaResponse* response) {
  auto result = registry_->GetIndexSchema(request->key_type());
  if (!result.ok()) {
    if (absl::IsNotFound(result.status())) {
      return AbslToGrpcStatus(
          MakeFetchIndexError(absl::StatusCode::kNotFound, result.status().message(), FetchIndexError::INDEX_NOT_FOUND, request->key_type()));
    }
    return AbslToGrpcStatus(result.status());
  }

  response->set_index_definition_id(result->index_definition_id);
  response->set_key_type(result->key_type);

  for (const auto& key_field : result->key_fields) {
    response->add_key_fields(key_field);
  }

  for (const auto& order_field : result->order_fields) {
    *response->add_order_fields() = order_field;
  }

  response->set_unique(result->unique);
  *response->mutable_index_descriptor_set() = result->index_descriptor_set;
  response->set_key_message_name(result->key_message_name);
  response->set_value_message_name(result->value_message_name);
  response->set_index_message_name(result->index_message_name);

  return grpc::Status::OK;
}

} // namespace artifact_system::service

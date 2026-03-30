#include "service/artifact_service_impl.h"

#include <stdexcept>

#include "service/grpc_error_util.h"

namespace artifact_system::service {

ArtifactServiceImpl::ArtifactServiceImpl(artifact::ArtifactStore* store) : store_(store) {
}

grpc::Status ArtifactServiceImpl::CreateArtifact(grpc::ServerContext* /*context*/, const CreateArtifactRequest* request, CreateArtifactResponse* response) {
  std::optional<std::string> transaction_id;
  if (request->has_transaction_id()) {
    transaction_id = request->transaction_id();
  }

  absl::StatusOr<artifact::CreateResult> result;
  try {
    result = store_->CreateArtifact(request->version_id(), request->payload(), transaction_id);
  } catch (const std::runtime_error& e) {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, e.what());
  }

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  response->set_artifact_id(result->artifact_id);
  response->set_snapshot_id(result->snapshot_id);
  return grpc::Status::OK;
}

grpc::Status ArtifactServiceImpl::GetArtifact(grpc::ServerContext* /*context*/, const GetArtifactRequest* request, GetArtifactResponse* response) {
  auto result = store_->GetArtifact(request->artifact_id(), request->context());

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  response->set_artifact_id(result->artifact_id);
  response->set_type_name(result->type_name);
  response->set_version_id(result->version_id);
  response->set_payload(result->payload);
  return grpc::Status::OK;
}

grpc::Status ArtifactServiceImpl::BatchGetArtifacts(grpc::ServerContext* /*context*/, const BatchGetArtifactsRequest* request,
                                                    BatchGetArtifactsResponse* response) {
  std::vector<uint64_t> artifact_ids(request->artifact_ids().begin(), request->artifact_ids().end());

  auto result = store_->BatchGetArtifacts(artifact_ids, request->context());

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  for (const auto& entry : *result) {
    auto* artifact_result = response->add_results();
    if (auto* get_result = std::get_if<artifact::GetResult>(&entry.result)) {
      auto* artifact = artifact_result->mutable_artifact();
      artifact->set_artifact_id(get_result->artifact_id);
      artifact->set_type_name(get_result->type_name);
      artifact->set_version_id(get_result->version_id);
      artifact->set_payload(get_result->payload);
    } else if (auto* not_found = std::get_if<ArtifactNotFoundError>(&entry.result)) {
      *artifact_result->mutable_not_found() = *not_found;
    }
  }

  return grpc::Status::OK;
}

grpc::Status ArtifactServiceImpl::UpdateArtifact(grpc::ServerContext* /*context*/, const UpdateArtifactRequest* request, UpdateArtifactResponse* response) {
  std::optional<std::string> transaction_id;
  if (request->has_transaction_id()) {
    transaction_id = request->transaction_id();
  }

  auto result = store_->UpdateArtifact(request->artifact_id(), request->version_id(), request->payload(), transaction_id);

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  response->set_snapshot_id(result->snapshot_id);
  return grpc::Status::OK;
}

grpc::Status ArtifactServiceImpl::DeleteArtifact(grpc::ServerContext* /*context*/, const DeleteArtifactRequest* request, DeleteArtifactResponse* response) {
  std::optional<std::string> transaction_id;
  if (request->has_transaction_id()) {
    transaction_id = request->transaction_id();
  }

  auto result = store_->DeleteArtifact(request->artifact_id(), transaction_id);

  if (!result.ok()) {
    return AbslToGrpcStatus(result.status());
  }

  response->set_snapshot_id(result->snapshot_id);
  return grpc::Status::OK;
}

} // namespace artifact_system::service

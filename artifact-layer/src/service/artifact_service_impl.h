#pragma once

#include "artifact/artifact_store.h"
#include "artifact_service.grpc.pb.h"

namespace artifact_system::service {

class ArtifactServiceImpl final : public ArtifactService::Service {
public:
  explicit ArtifactServiceImpl(artifact::ArtifactStore* store);

  grpc::Status CreateArtifact(grpc::ServerContext* context, const CreateArtifactRequest* request, CreateArtifactResponse* response) override;

  grpc::Status GetArtifact(grpc::ServerContext* context, const GetArtifactRequest* request, GetArtifactResponse* response) override;

  grpc::Status BatchGetArtifacts(grpc::ServerContext* context, const BatchGetArtifactsRequest* request, BatchGetArtifactsResponse* response) override;

  grpc::Status UpdateArtifact(grpc::ServerContext* context, const UpdateArtifactRequest* request, UpdateArtifactResponse* response) override;

  grpc::Status DeleteArtifact(grpc::ServerContext* context, const DeleteArtifactRequest* request, DeleteArtifactResponse* response) override;

private:
  artifact::ArtifactStore* store_;
};

} // namespace artifact_system::service

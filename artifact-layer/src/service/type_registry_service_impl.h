#ifndef ARTIFACT_SYSTEM_SERVICE_TYPE_REGISTRY_SERVICE_IMPL_H_
#define ARTIFACT_SYSTEM_SERVICE_TYPE_REGISTRY_SERVICE_IMPL_H_

#include "artifact/artifact_store.h"
#include "artifact_service.grpc.pb.h"
#include "registry/type_registry.h"

namespace artifact_system::service {

class TypeRegistryServiceImpl final : public TypeRegistryService::Service {
public:
  TypeRegistryServiceImpl(registry::TypeRegistry* registry, artifact::ArtifactStore* artifact_store);

  grpc::Status RegisterTypeVersion(grpc::ServerContext* context, const RegisterTypeVersionRequest* request, RegisterTypeVersionResponse* response) override;

  grpc::Status GetTypeVersion(grpc::ServerContext* context, const GetTypeVersionRequest* request, GetTypeVersionResponse* response) override;

  grpc::Status ListTypeVersions(grpc::ServerContext* context, const ListTypeVersionsRequest* request, ListTypeVersionsResponse* response) override;

  grpc::Status GetIndexSchema(grpc::ServerContext* context, const GetIndexSchemaRequest* request, GetIndexSchemaResponse* response) override;

private:
  registry::TypeRegistry* registry_;
  artifact::ArtifactStore* artifact_store_;
};

} // namespace artifact_system::service

#endif // ARTIFACT_SYSTEM_SERVICE_TYPE_REGISTRY_SERVICE_IMPL_H_

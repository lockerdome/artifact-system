#pragma once

#include <cstdint>
#include <string>

namespace id_allocator {

struct ServerConfig {
    std::string listen_address = "0.0.0.0:50051";

    // "memory" or "datastore"
    std::string store_type = "memory";

    // Datastore-specific configuration.
    std::string gcp_project_id;
    std::string datastore_endpoint;  // empty = production default
};

}  // namespace id_allocator

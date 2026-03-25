cmake_minimum_required(VERSION 3.24)
project(artifact_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# ── Dependencies (gRPC, protobuf, googletest) ─────────────────────────────
include(cmake/dependencies.cmake)

# ── Proto compilation ─────────────────────────────────────────────────────
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}")

# Well-known protobuf imports (google/protobuf/descriptor.proto, etc.)
set(_PROTOBUF_IMPORT_DIR "${protobuf_SOURCE_DIR}/src")

# Helper: generate C++ stubs from a .proto file (no gRPC).
# Usage: proto_generate(<base_name> <depends...>)
function(proto_generate BASE_NAME)
  set(PROTO_FILE "${PROTO_DIR}/${BASE_NAME}.proto")
  set(SRCS "${PROTO_OUT}/${BASE_NAME}.pb.cc")
  set(HDRS "${PROTO_OUT}/${BASE_NAME}.pb.h")
  add_custom_command(
    OUTPUT "${SRCS}" "${HDRS}"
    COMMAND ${_PROTOBUF_PROTOC}
      --cpp_out "${PROTO_OUT}"
      -I "${PROTO_DIR}"
      -I "${_PROTOBUF_IMPORT_DIR}"
      "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" ${ARGN} protoc
    COMMENT "Generating C++ stubs from ${BASE_NAME}.proto"
  )
  set(${BASE_NAME}_PROTO_SRCS "${SRCS}" PARENT_SCOPE)
  set(${BASE_NAME}_PROTO_HDRS "${HDRS}" PARENT_SCOPE)
endfunction()

# Helper: generate C++ + gRPC stubs from a .proto file.
function(proto_generate_grpc BASE_NAME)
  set(PROTO_FILE "${PROTO_DIR}/${BASE_NAME}.proto")
  set(PB_SRCS "${PROTO_OUT}/${BASE_NAME}.pb.cc")
  set(PB_HDRS "${PROTO_OUT}/${BASE_NAME}.pb.h")
  set(GRPC_SRCS "${PROTO_OUT}/${BASE_NAME}.grpc.pb.cc")
  set(GRPC_HDRS "${PROTO_OUT}/${BASE_NAME}.grpc.pb.h")
  add_custom_command(
    OUTPUT "${PB_SRCS}" "${PB_HDRS}" "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND ${_PROTOBUF_PROTOC}
      --grpc_out "${PROTO_OUT}"
      --cpp_out  "${PROTO_OUT}"
      -I "${PROTO_DIR}"
      -I "${_PROTOBUF_IMPORT_DIR}"
      --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN}"
      "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}" ${ARGN} protoc grpc_cpp_plugin
    COMMENT "Generating C++ + gRPC stubs from ${BASE_NAME}.proto"
  )
  set(${BASE_NAME}_PROTO_SRCS "${PB_SRCS}" PARENT_SCOPE)
  set(${BASE_NAME}_PROTO_HDRS "${PB_HDRS}" PARENT_SCOPE)
  set(${BASE_NAME}_GRPC_SRCS  "${GRPC_SRCS}" PARENT_SCOPE)
  set(${BASE_NAME}_GRPC_HDRS  "${GRPC_HDRS}" PARENT_SCOPE)
endfunction()

# Proto dependency chain:
#   artifact_options.proto  (imports google/protobuf/descriptor.proto)
#   artifact_types.proto    (imports artifact_options.proto, google/protobuf/descriptor.proto)
#   artifact_service.proto  (imports artifact_options.proto, google/protobuf/descriptor.proto)
#   artifact_internal.proto (no local imports)

set(OPTIONS_PROTO "${PROTO_DIR}/artifact_options.proto")
set(TYPES_PROTO   "${PROTO_DIR}/artifact_types.proto")
set(SERVICE_PROTO "${PROTO_DIR}/artifact_service.proto")
set(INTERNAL_PROTO "${PROTO_DIR}/artifact_internal.proto")

proto_generate(artifact_options)
proto_generate(artifact_types   "${OPTIONS_PROTO}")
proto_generate(artifact_internal)
proto_generate_grpc(artifact_service "${OPTIONS_PROTO}")

# ── Library: artifact_layer_proto ─────────────────────────────────────────
add_library(artifact_layer_proto
  ${artifact_options_PROTO_SRCS}
  ${artifact_types_PROTO_SRCS}
  ${artifact_internal_PROTO_SRCS}
  ${artifact_service_PROTO_SRCS}
  ${artifact_service_GRPC_SRCS}
)
target_include_directories(artifact_layer_proto PUBLIC "${PROTO_OUT}")
target_link_libraries(artifact_layer_proto PUBLIC ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})

# ── Find system libcurl (used by LakeFS storage client) ───────────────────
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# ── Library: artifact_layer_lib (core logic) ──────────────────────────────
add_library(artifact_layer_lib
  src/encoding/artifact_path.cpp
  src/encoding/base64url.cpp
  src/encoding/index_key_encoder.cpp
  src/index/index_schema_generator.cpp
  src/index/index_object.cpp
  src/index/index_merge.cpp
  src/index/index_derivation.cpp
  src/storage/memory_storage.cpp
  src/storage/lakefs_storage.cpp
)
target_include_directories(artifact_layer_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artifact_layer_lib
  PUBLIC
    artifact_layer_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    absl::status
    absl::statusor
    absl::strings
    Threads::Threads
    CURL::libcurl
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(artifact_layer_service src/main.cpp)
target_link_libraries(artifact_layer_service PRIVATE artifact_layer_lib)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

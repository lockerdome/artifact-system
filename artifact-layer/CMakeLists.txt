cmake_minimum_required(VERSION 3.24)
project(artifact_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# ── Dependencies (gRPC, protobuf, googletest) ─────────────────────────────
include(cmake/dependencies.cmake)

# ── Proto: artifact_system.proto ──────────────────────────────────────────
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")

set(AS_SYSTEM_PROTO "${PROTO_DIR}/artifact_system.proto")
set(AS_SYSTEM_PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/artifact_system.pb.cc")
set(AS_SYSTEM_PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/artifact_system.pb.h")

add_custom_command(
  OUTPUT "${AS_SYSTEM_PROTO_SRCS}" "${AS_SYSTEM_PROTO_HDRS}"
  COMMAND ${_PROTOBUF_PROTOC}
    --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${PROTO_DIR}"
    "${AS_SYSTEM_PROTO}"
  DEPENDS "${AS_SYSTEM_PROTO}"
  COMMENT "Generating C++ stubs from artifact_system.proto"
)

# ── Proto: artifact_service.proto ─────────────────────────────────────────
set(AS_SERVICE_PROTO "${PROTO_DIR}/artifact_service.proto")
set(AS_SERVICE_PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/artifact_service.pb.cc")
set(AS_SERVICE_PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/artifact_service.pb.h")
set(AS_SERVICE_GRPC_SRCS  "${CMAKE_CURRENT_BINARY_DIR}/artifact_service.grpc.pb.cc")
set(AS_SERVICE_GRPC_HDRS  "${CMAKE_CURRENT_BINARY_DIR}/artifact_service.grpc.pb.h")

add_custom_command(
  OUTPUT "${AS_SERVICE_PROTO_SRCS}" "${AS_SERVICE_PROTO_HDRS}"
         "${AS_SERVICE_GRPC_SRCS}" "${AS_SERVICE_GRPC_HDRS}"
  COMMAND ${_PROTOBUF_PROTOC}
    --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
    --cpp_out  "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${PROTO_DIR}"
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN}"
    "${AS_SERVICE_PROTO}"
  DEPENDS "${AS_SERVICE_PROTO}" "${AS_SYSTEM_PROTO}"
  COMMENT "Generating C++ + gRPC stubs from artifact_service.proto"
)

# ── Library: artifact_layer_proto ─────────────────────────────────────────
add_library(artifact_layer_proto
  ${AS_SYSTEM_PROTO_SRCS}
  ${AS_SERVICE_PROTO_SRCS}
  ${AS_SERVICE_GRPC_SRCS}
)
target_include_directories(artifact_layer_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(artifact_layer_proto PUBLIC ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})

# ── Library: artifact_layer_lib (core logic) ──────────────────────────────
# Placeholder — no source files yet. Header-only for now (id allocator interface).
add_library(artifact_layer_lib INTERFACE)
target_include_directories(artifact_layer_lib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(artifact_layer_lib
  INTERFACE
    artifact_layer_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    Threads::Threads
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(artifact_layer_service src/main.cpp)
target_link_libraries(artifact_layer_service PRIVATE artifact_layer_lib)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

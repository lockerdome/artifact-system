cmake_minimum_required(VERSION 3.24)
project(id_allocator_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# ── Dependencies (gRPC, protobuf, googleapis, googletest) ─────────────────
include(cmake/dependencies.cmake)

# ── Proto: id_allocator.proto ─────────────────────────────────────────────
get_filename_component(ID_ALLOC_PROTO
  "${CMAKE_CURRENT_SOURCE_DIR}/../proto/id_allocator.proto" ABSOLUTE)
get_filename_component(ID_ALLOC_PROTO_PATH "${ID_ALLOC_PROTO}" PATH)

set(ID_ALLOC_PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/id_allocator.pb.cc")
set(ID_ALLOC_PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/id_allocator.pb.h")
set(ID_ALLOC_GRPC_SRCS  "${CMAKE_CURRENT_BINARY_DIR}/id_allocator.grpc.pb.cc")
set(ID_ALLOC_GRPC_HDRS  "${CMAKE_CURRENT_BINARY_DIR}/id_allocator.grpc.pb.h")

add_custom_command(
  OUTPUT "${ID_ALLOC_PROTO_SRCS}" "${ID_ALLOC_PROTO_HDRS}"
         "${ID_ALLOC_GRPC_SRCS}" "${ID_ALLOC_GRPC_HDRS}"
  COMMAND ${_PROTOBUF_PROTOC}
    --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
    --cpp_out  "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${ID_ALLOC_PROTO_PATH}"
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN}"
    "${ID_ALLOC_PROTO}"
  DEPENDS "${ID_ALLOC_PROTO}"
  COMMENT "Generating C++ stubs from id_allocator.proto"
)

# ── Proto: google/datastore/v1 (for DatastoreStore) ──────────────────────
set(GOOGLEAPIS_DIR "${googleapis_SOURCE_DIR}")

# Datastore protos and their transitive dependencies
set(DS_PROTO_DIR "${GOOGLEAPIS_DIR}/google/datastore/v1")
set(DS_PROTO_FILES
  "${DS_PROTO_DIR}/datastore.proto"
  "${DS_PROTO_DIR}/entity.proto"
  "${DS_PROTO_DIR}/query.proto"
  "${DS_PROTO_DIR}/query_profile.proto"
  "${DS_PROTO_DIR}/aggregation_result.proto"
)

# Collect all the google/api/*.proto and google/type/*.proto that datastore
# imports transitively.  protoc resolves them via -I so we only need to
# compile the datastore protos themselves — the google/api and google/type
# protos are annotation-only (no services) but we still need their generated
# code for the C++ structs they define.
set(API_PROTO_FILES
  "${GOOGLEAPIS_DIR}/google/api/annotations.proto"
  "${GOOGLEAPIS_DIR}/google/api/client.proto"
  "${GOOGLEAPIS_DIR}/google/api/field_behavior.proto"
  "${GOOGLEAPIS_DIR}/google/api/http.proto"
  "${GOOGLEAPIS_DIR}/google/api/launch_stage.proto"
  "${GOOGLEAPIS_DIR}/google/api/routing.proto"
  "${GOOGLEAPIS_DIR}/google/type/latlng.proto"
)

set(DS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/googleapis")

# Build a list of generated .pb.cc / .grpc.pb.cc files.
set(DS_GENERATED_SRCS "")
foreach(proto_file IN LISTS DS_PROTO_FILES API_PROTO_FILES)
  # Compute the path relative to GOOGLEAPIS_DIR
  file(RELATIVE_PATH rel "${GOOGLEAPIS_DIR}" "${proto_file}")
  string(REPLACE ".proto" ".pb.cc" pb_cc "${rel}")
  string(REPLACE ".proto" ".pb.h" pb_h "${rel}")
  list(APPEND DS_GENERATED_SRCS "${DS_GENERATED_DIR}/${pb_cc}")
endforeach()

# Only generate gRPC stubs for the datastore service proto.
set(DS_GRPC_SRCS "${DS_GENERATED_DIR}/google/datastore/v1/datastore.grpc.pb.cc")
list(APPEND DS_GENERATED_SRCS "${DS_GRPC_SRCS}")

# Single custom command that generates all datastore proto outputs.
add_custom_command(
  OUTPUT ${DS_GENERATED_SRCS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DS_GENERATED_DIR}"
  # Generate protobuf messages for all protos
  COMMAND ${_PROTOBUF_PROTOC}
    --cpp_out "${DS_GENERATED_DIR}"
    -I "${GOOGLEAPIS_DIR}"
    -I "${grpc_SOURCE_DIR}/third_party/protobuf/src"
    ${DS_PROTO_FILES} ${API_PROTO_FILES}
  # Generate gRPC service stubs for the datastore service only
  COMMAND ${_PROTOBUF_PROTOC}
    --grpc_out "${DS_GENERATED_DIR}"
    -I "${GOOGLEAPIS_DIR}"
    -I "${grpc_SOURCE_DIR}/third_party/protobuf/src"
    --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN}"
    "${DS_PROTO_DIR}/datastore.proto"
  DEPENDS ${DS_PROTO_FILES} ${API_PROTO_FILES}
  COMMENT "Generating C++ stubs from google/datastore/v1 protos"
)

# ── Library: id_allocator_proto (our service proto stubs) ─────────────────
add_library(id_allocator_proto
  ${ID_ALLOC_PROTO_SRCS}
  ${ID_ALLOC_GRPC_SRCS}
)
target_include_directories(id_allocator_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(id_allocator_proto PUBLIC ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})

# ── Library: datastore_proto (Google Datastore proto stubs) ───────────────
add_library(datastore_proto ${DS_GENERATED_SRCS})
target_include_directories(datastore_proto PUBLIC "${DS_GENERATED_DIR}")
target_link_libraries(datastore_proto PUBLIC ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})

# ── Library: id_allocator_lib (core service logic) ────────────────────────
add_library(id_allocator_lib
  src/store/memory_store.cpp
  src/store/datastore_store.cpp
  src/allocator/bucket_selector.cpp
  src/allocator/super_block_allocator.cpp
  src/partition/partition_manager.cpp
  src/service/id_allocator_service.cpp
  src/service/id_allocator_admin_service.cpp
  src/server.cpp
)
target_include_directories(id_allocator_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(id_allocator_lib
  PUBLIC
    id_allocator_proto
    datastore_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    Threads::Threads
)

# ── Executable ────────────────────────────────────────────────────────────
add_executable(id_allocator_service src/main.cpp)
target_link_libraries(id_allocator_service PRIVATE id_allocator_lib)

# ── Tests ─────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

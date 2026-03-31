cmake_minimum_required(VERSION 3.24)
project(id_allocator_client_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(cmake/dependencies.cmake)

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

add_library(id_allocator_client_proto
  ${ID_ALLOC_PROTO_SRCS}
  ${ID_ALLOC_GRPC_SRCS}
)
target_include_directories(id_allocator_client_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(id_allocator_client_proto PUBLIC ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})

add_library(id_allocator_client_cpp_lib
  src/id_allocator_client/block.cpp
  src/id_allocator_client/block_double_buffer.cpp
  src/id_allocator_client/grpc_client.cpp
  src/id_allocator_client/client.cpp
)
target_include_directories(id_allocator_client_cpp_lib
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(id_allocator_client_cpp_lib
  PUBLIC
    id_allocator_client_proto
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF}
    Threads::Threads
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_subdirectory(tests)
endif()

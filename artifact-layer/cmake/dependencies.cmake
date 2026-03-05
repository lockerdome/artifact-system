include(FetchContent)

# ---------------------------------------------------------------------------
# gRPC  (brings protobuf + abseil + re2 + c-ares + zlib transitively)
# ---------------------------------------------------------------------------
FetchContent_Declare(
  grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG        v1.78.0
  GIT_SHALLOW    TRUE
)

# Tell gRPC's build to use its bundled deps rather than searching the system.
set(gRPC_ABSL_PROVIDER    "module" CACHE STRING "" FORCE)
set(gRPC_CARES_PROVIDER   "module" CACHE STRING "" FORCE)
set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_RE2_PROVIDER     "module" CACHE STRING "" FORCE)
set(gRPC_SSL_PROVIDER     "package" CACHE STRING "" FORCE)  # use system OpenSSL
set(gRPC_ZLIB_PROVIDER    "module" CACHE STRING "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)

# Disable install targets for protobuf and abseil to avoid export set conflicts.
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(grpc)

# Convenience aliases used throughout the build.
set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
set(_GRPC_CPP_PLUGIN $<TARGET_FILE:grpc_cpp_plugin>)
set(_PROTOBUF_LIBPROTOBUF libprotobuf)
set(_PROTOBUF_LIBPROTOC libprotoc)
set(_GRPC_GRPCPP grpc++)

# ---------------------------------------------------------------------------
# GoogleTest
# ---------------------------------------------------------------------------
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.16.0
  GIT_SHALLOW    TRUE
)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

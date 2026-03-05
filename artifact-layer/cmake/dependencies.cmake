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

# ---------------------------------------------------------------------------
# Sanitizer compatibility for GCC + gRPC v1.78.0 / abseil.
#
# Problem 1 (ASan): Abseil's hash_policy_traits.h triggers a constexpr
#   evaluation failure under GCC when compiled with -fsanitize=address.
# Problem 2 (TSan): protoc and grpc_cpp_plugin are build-time host tools
#   that must not be instrumented; TSan causes them to crash with
#   "unexpected memory mapping" before they can generate any code.
#
# Fix: Strip sanitizer flags from CMAKE_CXX_FLAGS before building
# dependencies, then restore them afterwards.  Our own targets still see
# the full flags because they are defined after the restore.
#
# Trade-off: dependencies are NOT instrumented, so sanitizers only catch
# issues in *our* code, not in gRPC/protobuf internals.  For a project
# that wraps these libraries this is acceptable — gRPC has its own
# sanitizer CI.
# ---------------------------------------------------------------------------
set(_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
set(_SAVED_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(_SAVED_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

string(REGEX REPLACE "-fsanitize=[^ ]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "-fno-sanitize-recover=[^ ]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "-fno-omit-frame-pointer" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "-fsanitize=[^ ]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-fno-sanitize-recover=[^ ]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-fno-omit-frame-pointer" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
string(REGEX REPLACE "-fsanitize=[^ ]+" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
string(REGEX REPLACE "-fsanitize=[^ ]+" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

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

# ---------------------------------------------------------------------------
# Restore sanitizer flags so our targets are instrumented.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${_SAVED_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${_SAVED_C_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${_SAVED_EXE_LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS "${_SAVED_SHARED_LINKER_FLAGS}")

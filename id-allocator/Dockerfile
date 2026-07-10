# Multi-stage image for id-allocator-service.
#
# Build context must be the id-allocator/ directory (this file's directory) so
# that both proto/ and id-allocator-service/ are reachable — the service's
# CMakeLists.txt compiles ../proto/id_allocator.proto.
#
#   cd id-allocator && docker build -t id-allocator-service .

# ---------------------------------------------------------------------------
# deps: pre-build the third-party gRPC stack (grpc++, protoc, grpc_cpp_plugin)
# from source. This layer only invalidates when the toolchain packages or the
# dependency pins in cmake/dependencies.cmake change — not when service source
# changes — so the expensive gRPC-from-source build is cached across rebuilds.
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS deps

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    ninja-build \
    git \
    pkg-config \
    libssl-dev \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /code/id-allocator-service

# Only the pieces needed to fetch and build the third-party dependencies.
COPY id-allocator-service/CMakePresets.json ./CMakePresets.json
COPY id-allocator-service/cmake/dependencies.cmake ./cmake/dependencies.cmake

# deps-CMakeLists.txt is a minimal project whose only job is to pull in
# cmake/dependencies.cmake. It mirrors the real CMakeLists.txt globals and uses
# the same release preset, so the gRPC build lands in build/release configured
# identically to the build stage and is reused there rather than rebuilt.
COPY deps-CMakeLists.txt ./CMakeLists.txt

RUN cmake --preset release \
 && cmake --build --preset release --target grpc++ grpc_cpp_plugin protoc

# ---------------------------------------------------------------------------
# build: compile id-allocator-service against the pre-built gRPC, then run the
# test suite so a broken build never produces a runtime image.
# ---------------------------------------------------------------------------
FROM deps AS build

# Overlay the real sources on top of the deps project. build/release (with the
# pre-built gRPC) is retained from the deps stage, so cmake reconfigures and
# reuses gRPC instead of rebuilding it. Sources are copied selectively so a
# stray local build/ directory can never clobber the cached build tree.
COPY proto/ /code/proto/
COPY id-allocator-service/CMakeLists.txt id-allocator-service/CMakePresets.json ./
COPY id-allocator-service/cmake/ ./cmake/
COPY id-allocator-service/src/ ./src/
COPY id-allocator-service/tests/ ./tests/

RUN cmake --preset release \
 && cmake --build --preset release \
 && ctest --preset release

# ---------------------------------------------------------------------------
# runtime: slim image carrying just the binary and its shared-library deps.
# gRPC is built with gRPC_SSL_PROVIDER=package, so the binary dynamically links
# the system OpenSSL runtime.
# ---------------------------------------------------------------------------
FROM debian:trixie-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3t64 \
  && rm -rf /var/lib/apt/lists/*

COPY --from=build \
  /code/id-allocator-service/build/release/id_allocator_service \
  /usr/local/bin/id_allocator_service

ENTRYPOINT ["id_allocator_service"]

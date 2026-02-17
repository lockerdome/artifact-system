FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    ninja-build \
    git \
    pkg-config \
    libssl-dev \
    libcurl4-openssl-dev \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /code

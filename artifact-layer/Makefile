CPP23_TOOLCHAIN_IMAGE ?= cpp23-toolchain
PRESET ?= release

REPO_ROOT := $(abspath ..)

DOCKER_RUN := docker run -it --rm \
	--mount type=bind,source=$(REPO_ROOT),target=/code \
	-w /code/artifact-layer \
	$(CPP23_TOOLCHAIN_IMAGE)

LAKEFS_LOCAL_DIR := dev/lakefs-local
LAKEFS_LOCAL_MAKE := $(MAKE) -C $(LAKEFS_LOCAL_DIR)

.PHONY: toolchain configure build test run lakefs-up lakefs-bootstrap lakefs-down lakefs-logs lakefs-status

toolchain:
	$(MAKE) -C $(REPO_ROOT) build_cpp23_toolchain CPP23_TOOLCHAIN_IMAGE=$(CPP23_TOOLCHAIN_IMAGE)

configure:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET)'

build:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET) && cmake --build --preset $(PRESET)'

test:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET) && cmake --build --preset $(PRESET) && ctest --preset $(PRESET)'

run:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET) && cmake --build --preset $(PRESET) && ./build/$(PRESET)/artifact_layer_service'

lakefs-up:
	$(LAKEFS_LOCAL_MAKE) up

lakefs-bootstrap:
	$(LAKEFS_LOCAL_MAKE) bootstrap

lakefs-down:
	$(LAKEFS_LOCAL_MAKE) down

lakefs-logs:
	$(LAKEFS_LOCAL_MAKE) logs

lakefs-status:
	$(LAKEFS_LOCAL_MAKE) status

CPP23_TOOLCHAIN_IMAGE ?= cpp23-toolchain
PRESET ?= release

ARTIFACT_SYSTEM_ROOT := $(abspath ..)
REPO_ROOT := $(abspath ../..)

DOCKER_RUN := docker run -it --rm \
	--mount type=bind,source=$(ARTIFACT_SYSTEM_ROOT),target=/code/artifact-system \
	--mount type=bind,source=$(REPO_ROOT)/id-allocator,target=/code/id-allocator \
	-w /code/artifact-system/artifact-layer \
	$(CPP23_TOOLCHAIN_IMAGE)

.PHONY: toolchain configure build test run

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

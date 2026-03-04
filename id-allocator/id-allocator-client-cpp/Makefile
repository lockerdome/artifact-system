CPP23_TOOLCHAIN_IMAGE ?= cpp23-toolchain
PRESET ?= release

ID_ALLOCATOR_ROOT := $(abspath ..)
REPO_ROOT := $(abspath ../..)

DOCKER_RUN := docker run -it --rm \
	--mount type=bind,source=$(ID_ALLOCATOR_ROOT),target=/code \
	-w /code/id-allocator-client-cpp \
	$(CPP23_TOOLCHAIN_IMAGE)

.PHONY: toolchain configure build test

toolchain:
	$(MAKE) -C $(REPO_ROOT) build_cpp23_toolchain CPP23_TOOLCHAIN_IMAGE=$(CPP23_TOOLCHAIN_IMAGE)

configure:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET)'

build:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET) && cmake --build --preset $(PRESET)'

test:
	$(DOCKER_RUN) /bin/sh -c 'cmake --preset $(PRESET) && cmake --build --preset $(PRESET) && ctest --preset $(PRESET)'

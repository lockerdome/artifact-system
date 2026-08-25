CPP23_TOOLCHAIN_IMAGE ?= cpp23-toolchain

.PHONY: build_cpp23_toolchain

build_cpp23_toolchain:
	docker build --tag $(CPP23_TOOLCHAIN_IMAGE) -f docker/cpp23/Dockerfile docker/cpp23

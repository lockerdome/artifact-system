LAKEFS_CONTAINER ?= artifact-layer-lakefs
LAKEFS_IMAGE ?= treeverse/lakefs:1.76.0

.PHONY: up bootstrap down logs status

up: down
	docker run -d --name '$(LAKEFS_CONTAINER)' \
		-p 8000:8000 \
		'$(LAKEFS_IMAGE)' run --quickstart >/dev/null
	$(MAKE) bootstrap

bootstrap:
	LAKEFS_ENDPOINT='$(LAKEFS_ENDPOINT)' \
	LAKEFS_ACCESS_KEY_ID='$(LAKEFS_ACCESS_KEY_ID)' \
	LAKEFS_SECRET_ACCESS_KEY='$(LAKEFS_SECRET_ACCESS_KEY)' \
	LAKEFS_REPOSITORY='$(LAKEFS_REPOSITORY)' \
	LAKEFS_STORAGE_NAMESPACE_PREFIX='$(LAKEFS_STORAGE_NAMESPACE_PREFIX)' \
	LAKEFS_CANONICAL_BRANCH='$(LAKEFS_CANONICAL_BRANCH)' \
	./bootstrap.sh

down:
	-docker rm -f '$(LAKEFS_CONTAINER)' >/dev/null 2>&1

logs:
	docker logs -f '$(LAKEFS_CONTAINER)'

status:
	@echo "LakeFS:" && docker ps --filter "name=^/$(LAKEFS_CONTAINER)$$" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"

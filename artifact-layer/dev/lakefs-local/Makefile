LAKEFS_ENDPOINT ?= http://localhost:8000
LAKEFS_ACCESS_KEY_ID ?= LKFS_LOCAL_ACCESS_KEY
LAKEFS_SECRET_ACCESS_KEY ?= LKFS_LOCAL_SECRET_KEY
LAKEFS_REPOSITORY ?= artifact-layer-it
LAKEFS_STORAGE_BUCKET ?= lakefs
LAKEFS_CANONICAL_BRANCH ?= main

NETWORK ?= artifact-layer-lakefs-net

POSTGRES_CONTAINER ?= artifact-layer-lakefs-postgres
MINIO_CONTAINER ?= artifact-layer-lakefs-minio
LAKEFS_CONTAINER ?= artifact-layer-lakefs

POSTGRES_VOLUME ?= artifact-layer-lakefs-postgres-data
MINIO_VOLUME ?= artifact-layer-lakefs-minio-data

POSTGRES_IMAGE ?= postgres:14
MINIO_IMAGE ?= minio/minio:RELEASE.2024-12-18T13-15-44Z
MINIO_MC_IMAGE ?= minio/mc:RELEASE.2024-11-21T17-21-54Z
LAKEFS_IMAGE ?= treeverse/lakefs:1

MINIO_ROOT_USER ?= minioadmin
MINIO_ROOT_PASSWORD ?= minioadmin
LAKEFS_AUTH_ENCRYPT_SECRET_KEY ?= 0123456789abcdef0123456789abcdef

.PHONY: up bootstrap down logs status

up: down
	docker network create '$(NETWORK)' >/dev/null
	docker volume create '$(POSTGRES_VOLUME)' >/dev/null
	docker volume create '$(MINIO_VOLUME)' >/dev/null
	docker run -d --name '$(POSTGRES_CONTAINER)' --restart unless-stopped --network '$(NETWORK)' \
		-e POSTGRES_USER=postgres \
		-e POSTGRES_PASSWORD=postgres \
		-e POSTGRES_DB=postgres \
		-v '$(POSTGRES_VOLUME)':/var/lib/postgresql/data \
		'$(POSTGRES_IMAGE)' >/dev/null
	docker run -d --name '$(MINIO_CONTAINER)' --restart unless-stopped --network '$(NETWORK)' \
		-e MINIO_ROOT_USER='$(MINIO_ROOT_USER)' \
		-e MINIO_ROOT_PASSWORD='$(MINIO_ROOT_PASSWORD)' \
		-p 9000:9000 \
		-p 9001:9001 \
		-v '$(MINIO_VOLUME)':/data \
		'$(MINIO_IMAGE)' server /data --console-address ':9001' >/dev/null
	docker run --rm --network '$(NETWORK)' \
		-e MINIO_ROOT_USER='$(MINIO_ROOT_USER)' \
		-e MINIO_ROOT_PASSWORD='$(MINIO_ROOT_PASSWORD)' \
		-e LAKEFS_STORAGE_BUCKET='$(LAKEFS_STORAGE_BUCKET)' \
		'$(MINIO_MC_IMAGE)' /bin/sh -c "until mc alias set local http://$(MINIO_CONTAINER):9000 \"\$$MINIO_ROOT_USER\" \"\$$MINIO_ROOT_PASSWORD\"; do sleep 1; done; mc mb --ignore-existing local/\$$LAKEFS_STORAGE_BUCKET"
	docker run -d --name '$(LAKEFS_CONTAINER)' --restart unless-stopped --network '$(NETWORK)' \
		-e LAKEFS_DATABASE_TYPE=postgres \
		-e LAKEFS_DATABASE_POSTGRES_CONNECTION_STRING='postgres://postgres:postgres@$(POSTGRES_CONTAINER):5432/postgres?sslmode=disable' \
		-e LAKEFS_AUTH_ENCRYPT_SECRET_KEY='$(LAKEFS_AUTH_ENCRYPT_SECRET_KEY)' \
		-e LAKEFS_BLOCKSTORE_TYPE=s3 \
		-e LAKEFS_BLOCKSTORE_S3_ENDPOINT='http://$(MINIO_CONTAINER):9000' \
		-e LAKEFS_BLOCKSTORE_S3_REGION=us-east-1 \
		-e LAKEFS_BLOCKSTORE_S3_DISCOVER_BUCKET_REGION=false \
		-e LAKEFS_BLOCKSTORE_S3_FORCE_PATH_STYLE=true \
		-e LAKEFS_BLOCKSTORE_S3_CREDENTIALS_ACCESS_KEY_ID='$(MINIO_ROOT_USER)' \
		-e LAKEFS_BLOCKSTORE_S3_CREDENTIALS_SECRET_ACCESS_KEY='$(MINIO_ROOT_PASSWORD)' \
		-e LAKEFS_LOGGING_LEVEL=INFO \
		-p 8000:8000 \
		'$(LAKEFS_IMAGE)' run >/dev/null
	$(MAKE) bootstrap

bootstrap:
	LAKEFS_ENDPOINT='$(LAKEFS_ENDPOINT)' \
	LAKEFS_ACCESS_KEY_ID='$(LAKEFS_ACCESS_KEY_ID)' \
	LAKEFS_SECRET_ACCESS_KEY='$(LAKEFS_SECRET_ACCESS_KEY)' \
	LAKEFS_REPOSITORY='$(LAKEFS_REPOSITORY)' \
	LAKEFS_STORAGE_BUCKET='$(LAKEFS_STORAGE_BUCKET)' \
	LAKEFS_CANONICAL_BRANCH='$(LAKEFS_CANONICAL_BRANCH)' \
	./bootstrap.sh

down:
	-docker rm -f '$(LAKEFS_CONTAINER)' >/dev/null 2>&1
	-docker rm -f '$(MINIO_CONTAINER)' >/dev/null 2>&1
	-docker rm -f '$(POSTGRES_CONTAINER)' >/dev/null 2>&1
	-docker network rm '$(NETWORK)' >/dev/null 2>&1
	-docker volume rm '$(POSTGRES_VOLUME)' >/dev/null 2>&1
	-docker volume rm '$(MINIO_VOLUME)' >/dev/null 2>&1

logs:
	docker logs -f '$(LAKEFS_CONTAINER)'

status:
	@echo "Postgres:" && docker ps --filter "name=^/$(POSTGRES_CONTAINER)$$" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
	@echo "MinIO:" && docker ps --filter "name=^/$(MINIO_CONTAINER)$$" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
	@echo "LakeFS:" && docker ps --filter "name=^/$(LAKEFS_CONTAINER)$$" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"

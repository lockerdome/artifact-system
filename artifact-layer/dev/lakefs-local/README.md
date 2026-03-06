# Local LakeFS for Integration Tests

This stack runs lakeFS against a local MinIO (S3-compatible) backend and
Postgres metadata store.

## Prerequisites

- Docker (`docker` CLI)
- `curl`

## Start and bootstrap

From `artifact-system/artifact-layer`:

```bash
make -C dev/lakefs-local up
```

The script starts:

- `postgres` (lakeFS metadata DB)
- `minio` (S3-compatible object store)
- `minio-init` (creates the `lakefs` bucket)
- `lakefs` (API at `http://localhost:8000`)

It then bootstraps an admin key and creates a test repository.

Default bootstrap values:

- `LAKEFS_ENDPOINT=http://localhost:8000`
- `LAKEFS_ACCESS_KEY_ID=LKFS_LOCAL_ACCESS_KEY`
- `LAKEFS_SECRET_ACCESS_KEY=LKFS_LOCAL_SECRET_KEY`
- `LAKEFS_REPOSITORY=artifact-layer-it`
- `LAKEFS_STORAGE_BUCKET=lakefs`
- `LAKEFS_CANONICAL_BRANCH=main`

You can override any of those via Make variables, for example:

```bash
make -C dev/lakefs-local \
  LAKEFS_ACCESS_KEY_ID=my-access-key \
  LAKEFS_SECRET_ACCESS_KEY=my-secret \
  up
```

If you prefer running from `artifact-system/artifact-layer`, shortcuts are
available:

```bash
make lakefs-up
make lakefs-down
```

## Run LakeFS conformance tests

```bash
export LAKEFS_ENDPOINT='http://localhost:8000'
export LAKEFS_ACCESS_KEY_ID='LKFS_LOCAL_ACCESS_KEY'
export LAKEFS_SECRET_ACCESS_KEY='LKFS_LOCAL_SECRET_KEY'
export LAKEFS_STORAGE_BUCKET='lakefs'
export LAKEFS_CANONICAL_BRANCH='main'

cmake --preset release
cmake --build --preset release
ctest --preset release -R '^LakeFS/'
```

`tests/lakefs_storage_test.cpp` creates a unique repository per test case and
cleans up repositories at test teardown.

## Stop and clean volumes

```bash
make -C dev/lakefs-local down
```

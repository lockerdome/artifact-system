# Local LakeFS for Integration Tests

This flow runs lakeFS in quickstart mode using a single Docker container.
The setup is intentionally ephemeral for test runs.

## Prerequisites

- Docker (`docker` CLI)
- `curl`

## Start and bootstrap

From `artifact-system/artifact-layer`:

```bash
make -C dev/lakefs-local up
```

The script starts:

- `lakefs` in quickstart mode (API at `http://localhost:8000`)

It then creates the test repository.
Each `up` run starts from a clean instance.

Default bootstrap values:

- `LAKEFS_ENDPOINT=http://localhost:8000`
- `LAKEFS_ACCESS_KEY_ID=AKIAIOSFOLQUICKSTART`
- `LAKEFS_SECRET_ACCESS_KEY=wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY`
- `LAKEFS_REPOSITORY=artifact-layer-it`
- `LAKEFS_STORAGE_NAMESPACE_PREFIX=local://`
- `LAKEFS_CANONICAL_BRANCH=main`

You can override those via Make variables, for example:

```bash
make -C dev/lakefs-local \
  LAKEFS_REPOSITORY=my-it-repository \
  LAKEFS_STORAGE_NAMESPACE_PREFIX=local://my-it/ \
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
export LAKEFS_ACCESS_KEY_ID='AKIAIOSFOLQUICKSTART'
export LAKEFS_SECRET_ACCESS_KEY='wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY'
export LAKEFS_STORAGE_NAMESPACE_PREFIX='local://'
export LAKEFS_CANONICAL_BRANCH='main'

cmake --preset release
cmake --build --preset release
ctest --preset release -R '^LakeFS/'
```

`tests/lakefs_storage_test.cpp` creates a unique repository per test case and
cleans up repositories at test teardown.

## Stop

```bash
make -C dev/lakefs-local down
```

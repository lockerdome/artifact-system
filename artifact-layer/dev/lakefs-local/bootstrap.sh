#!/usr/bin/env bash

set -euo pipefail

LAKEFS_ENDPOINT="${LAKEFS_ENDPOINT:-http://localhost:8000}"
LAKEFS_ACCESS_KEY_ID="${LAKEFS_ACCESS_KEY_ID:-LKFS_LOCAL_ACCESS_KEY}"
LAKEFS_SECRET_ACCESS_KEY="${LAKEFS_SECRET_ACCESS_KEY:-LKFS_LOCAL_SECRET_KEY}"
LAKEFS_REPOSITORY="${LAKEFS_REPOSITORY:-artifact-layer-it}"
LAKEFS_STORAGE_BUCKET="${LAKEFS_STORAGE_BUCKET:-lakefs}"
LAKEFS_CANONICAL_BRANCH="${LAKEFS_CANONICAL_BRANCH:-main}"

wait_for_lakefs() {
  local attempts=0
  local max_attempts=60

  until curl -fsS "${LAKEFS_ENDPOINT}/api/v1/healthcheck" >/dev/null 2>&1; do
    attempts=$((attempts + 1))
    if [ "$attempts" -ge "$max_attempts" ]; then
      echo "LakeFS did not become healthy at ${LAKEFS_ENDPOINT}" >&2
      return 1
    fi
    sleep 1
  done
}

post_json() {
  local url="$1"
  local userpwd="$2"
  local json_body="$3"

  curl -sS -o /tmp/lakefs_bootstrap_response.json -w "%{http_code}" \
    -u "$userpwd" \
    -X POST "$url" \
    -H "Content-Type: application/json" \
    -H "Accept: application/json" \
    -d "$json_body"
}

setup_lakefs() {
  local setup_payload
  setup_payload=$(cat <<EOF
{"username":"local-admin","key":{"access_key_id":"${LAKEFS_ACCESS_KEY_ID}","secret_access_key":"${LAKEFS_SECRET_ACCESS_KEY}"}}
EOF
)

  local code
  code=$(curl -sS -o /tmp/lakefs_bootstrap_response.json -w "%{http_code}" \
    -X POST "${LAKEFS_ENDPOINT}/api/v1/setup_lakefs" \
    -H "Content-Type: application/json" \
    -H "Accept: application/json" \
    -d "$setup_payload")

  case "$code" in
    200|201)
      echo "LakeFS bootstrap user created."
      ;;
    400|409)
      echo "LakeFS already initialized; reusing existing setup."
      ;;
    *)
      echo "Failed to initialize LakeFS (HTTP ${code})." >&2
      cat /tmp/lakefs_bootstrap_response.json >&2 || true
      return 1
      ;;
  esac
}

create_repository() {
  local repo_payload
  repo_payload=$(cat <<EOF
{"name":"${LAKEFS_REPOSITORY}","storage_namespace":"s3://${LAKEFS_STORAGE_BUCKET}/${LAKEFS_REPOSITORY}","default_branch":"${LAKEFS_CANONICAL_BRANCH}"}
EOF
)

  local code
  code=$(post_json "${LAKEFS_ENDPOINT}/api/v1/repositories" "${LAKEFS_ACCESS_KEY_ID}:${LAKEFS_SECRET_ACCESS_KEY}" "$repo_payload")

  case "$code" in
    200|201)
      echo "LakeFS repository '${LAKEFS_REPOSITORY}' created."
      ;;
    409)
      echo "LakeFS repository '${LAKEFS_REPOSITORY}' already exists."
      ;;
    *)
      echo "Failed to create repository '${LAKEFS_REPOSITORY}' (HTTP ${code})." >&2
      cat /tmp/lakefs_bootstrap_response.json >&2 || true
      return 1
      ;;
  esac
}

wait_for_lakefs
setup_lakefs
create_repository

cat <<EOF

LakeFS local environment is ready.

Export these vars for integration tests:
  export LAKEFS_ENDPOINT='${LAKEFS_ENDPOINT}'
  export LAKEFS_ACCESS_KEY_ID='${LAKEFS_ACCESS_KEY_ID}'
  export LAKEFS_SECRET_ACCESS_KEY='${LAKEFS_SECRET_ACCESS_KEY}'
  export LAKEFS_STORAGE_BUCKET='${LAKEFS_STORAGE_BUCKET}'
  export LAKEFS_CANONICAL_BRANCH='${LAKEFS_CANONICAL_BRANCH}'

Run LakeFS conformance tests with:
  ctest --preset release -R '^LakeFS/'
EOF

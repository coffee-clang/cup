#!/usr/bin/env bash

# Purpose: Verifies exact Tests-run selection and rejection of stale or failed runs.
set -euo pipefail

TESTS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$TESTS_ROOT/support/common.sh"

test_begin tests-run
MOCK_BIN="$TMP_ROOT/bin"
mkdir -p "$MOCK_BIN"
SHA=0123456789abcdef0123456789abcdef01234567
OTHER_SHA=1111111111111111111111111111111111111111

# Simulated GitHub API responses cover success, stale runs and wrong workflow ownership.
cat > "$MOCK_BIN/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:-}" = api ] || exit 2
endpoint=${2:-}
case "$endpoint" in
    *'/actions/workflows/tests.yml/runs?'*)
        cat <<JSON
{
  "workflow_runs": [
    {
      "id": 40,
      "name": "Tests",
      "path": ".github/workflows/tests.yml",
      "head_sha": "$OTHER_SHA",
      "head_branch": "main",
      "status": "completed",
      "conclusion": "success",
      "event": "push",
      "run_number": 40,
      "run_attempt": 1
    },
    {
      "id": 41,
      "name": "Tests",
      "path": ".github/workflows/tests.yml",
      "head_sha": "$SHA",
      "head_branch": "main",
      "status": "completed",
      "conclusion": "success",
      "event": "push",
      "run_number": 41,
      "run_attempt": 2
    },
    {
      "id": 42,
      "name": "Tests",
      "path": ".github/workflows/tests.yml",
      "head_sha": "$SHA",
      "head_branch": "main",
      "status": "completed",
      "conclusion": "failure",
      "event": "workflow_dispatch",
      "run_number": 42,
      "run_attempt": 1
    }
  ]
}
JSON
        ;;
    *'/actions/runs/41')
        cat <<JSON
{
  "id": 41,
  "name": "Tests",
  "path": ".github/workflows/tests.yml",
  "head_sha": "$SHA",
  "head_branch": "main",
  "status": "completed",
  "conclusion": "success",
  "event": "push",
  "run_number": 41,
  "run_attempt": 2
}
JSON
        ;;
    *'/actions/runs/42')
        cat <<JSON
{
  "id": 42,
  "name": "Tests",
  "path": ".github/workflows/tests.yml",
  "head_sha": "$SHA",
  "head_branch": "main",
  "status": "completed",
  "conclusion": "failure",
  "event": "workflow_dispatch",
  "run_number": 42,
  "run_attempt": 1
}
JSON
        ;;
    *'/actions/runs/43')
        cat <<JSON
{
  "id": 43,
  "name": "Other",
  "path": ".github/workflows/other.yml",
  "head_sha": "$SHA",
  "head_branch": "main",
  "status": "completed",
  "conclusion": "success",
  "event": "push",
  "run_number": 43,
  "run_attempt": 1
}
JSON
        ;;
    *) exit 2 ;;
esac
MOCK
chmod +x "$MOCK_BIN/gh"

# Invoke the real resolver against the controlled API boundary.
run_resolver() {
    output=$1
    shift
    PATH="$MOCK_BIN:$PATH" GH_TOKEN=test SOURCE_REPOSITORY=example/private \
        SHA="$SHA" OTHER_SHA="$OTHER_SHA" GITHUB_OUTPUT="$output" "$@" \
        "$PROJECT_ROOT/scripts/release/resolve-tests-run.sh"
}

run_resolver "$TMP_ROOT/auto.out" env >/dev/null
grep -Fx 'run_id=41' "$TMP_ROOT/auto.out" >/dev/null
grep -Fx 'run_attempt=2' "$TMP_ROOT/auto.out" >/dev/null

# Explicit run ids must still match workflow, branch, commit and conclusion.
if run_resolver "$TMP_ROOT/failed.out" env TESTS_RUN_ID=42 >/dev/null 2>&1; then
    fail 'failed Tests run unexpectedly passed release provenance validation'
fi
if run_resolver "$TMP_ROOT/wrong-workflow.out" env TESTS_RUN_ID=43 >/dev/null 2>&1; then
    fail 'another workflow unexpectedly passed Tests provenance validation'
fi
if run_resolver "$TMP_ROOT/invalid-id.out" env TESTS_RUN_ID='41x' >/dev/null 2>&1; then
    fail 'invalid Tests run id unexpectedly passed validation'
fi

printf 'Tests run provenance tests passed.\n'

#!/usr/bin/env bash

# Purpose: Resolves and validates one successful Tests workflow run for the
# exact main-branch source commit selected by release.yml.
set -euo pipefail

: "${GH_TOKEN:?GH_TOKEN is required}"
: "${SOURCE_REPOSITORY:?SOURCE_REPOSITORY is required}"
: "${SHA:?SHA is required}"
: "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"

[[ "$SOURCE_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
    echo "Invalid SOURCE_REPOSITORY: $SOURCE_REPOSITORY" >&2
    exit 1
}
[[ "$SHA" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Invalid SHA: $SHA" >&2
    exit 1
}

# Resolve a successful Tests run for the exact main-branch commit.
run_id=${TESTS_RUN_ID:-}
if [[ -z "$run_id" ]]; then
    response=$(gh api \
        "repos/$SOURCE_REPOSITORY/actions/workflows/tests.yml/runs?branch=main&status=completed&per_page=100")
    run_id=$(jq -r --arg sha "$SHA" '
        [.workflow_runs[]
            | select(.head_sha == $sha)
            | select(.head_branch == "main")
            | select(.conclusion == "success")
            | select(.event == "push" or .event == "workflow_dispatch")]
        | sort_by(.run_number)
        | last
        | .id // empty
    ' <<<"$response")
    [[ -n "$run_id" ]] || {
        echo "No successful Tests run was found for main commit $SHA." >&2
        exit 1
    }
fi
[[ "$run_id" =~ ^[1-9][0-9]*$ ]] || {
    echo "Invalid Tests run id: $run_id" >&2
    exit 1
}

# Re-read the selected run and validate provenance before downloading artifacts.
run=$(gh api "repos/$SOURCE_REPOSITORY/actions/runs/$run_id")
readarray -t fields < <(jq -r '[
    (.name // ""),
    (.path // ""),
    (.head_sha // ""),
    (.head_branch // ""),
    (.status // ""),
    (.conclusion // ""),
    (.event // ""),
    (.run_attempt // 0 | tostring)
] | .[]' <<<"$run")

[[ ${fields[0]} == Tests ]] || {
    echo "Run $run_id is not the Tests workflow." >&2
    exit 1
}
[[ ${fields[1]} == .github/workflows/tests.yml ]] || {
    echo "Run $run_id uses an unexpected workflow path: ${fields[1]}" >&2
    exit 1
}
[[ ${fields[2]} == "$SHA" ]] || {
    echo "Run $run_id tested ${fields[2]} instead of $SHA." >&2
    exit 1
}
[[ ${fields[3]} == main ]] || {
    echo "Run $run_id did not test the main branch." >&2
    exit 1
}
[[ ${fields[4]} == completed && ${fields[5]} == success ]] || {
    echo "Run $run_id is not a completed successful Tests run." >&2
    exit 1
}
[[ ${fields[6]} == push || ${fields[6]} == workflow_dispatch ]] || {
    echo "Run $run_id has an unsupported event: ${fields[6]}" >&2
    exit 1
}

printf 'run_id=%s\n' "$run_id" >> "$GITHUB_OUTPUT"
printf 'run_attempt=%s\n' "${fields[7]}" >> "$GITHUB_OUTPUT"
printf 'Using successful Tests run %s (attempt %s) for %s.\n' \
    "$run_id" "${fields[7]}" "$SHA"

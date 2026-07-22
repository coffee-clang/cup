#!/usr/bin/env bash

# Purpose: Verifies release-candidate decisions against the public repository,
# including draft recovery and query failures.
set -euo pipefail

TESTS_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
. "$TESTS_ROOT/support/common.sh"

test_begin release-prepare
REPO="$TMP_ROOT/repo"
MOCK_BIN="$TMP_ROOT/bin"
MOCK_STATE="$TMP_ROOT/state"
mkdir -p "$REPO/scripts/release" "$MOCK_BIN" "$MOCK_STATE"
cp "$PROJECT_ROOT/VERSION" "$REPO/VERSION"
cp "$PROJECT_ROOT/scripts/version.sh" "$REPO/scripts/version.sh"
cp "$PROJECT_ROOT/scripts/release/common.sh" "$REPO/scripts/release/common.sh"
cp "$PROJECT_ROOT/scripts/release/prepare.sh" "$REPO/scripts/release/prepare.sh"
chmod +x "$REPO/scripts/version.sh" "$REPO/scripts/release/prepare.sh"
(
    cd "$REPO"
    git init -q
    git config user.name test
    git config user.email test@example.invalid
    git add .
    git commit -qm initial
)

cat > "$MOCK_BIN/gh" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:-}" = release ] && [ "${2:-}" = view ] || exit 2
printf '%s\n' "$*" > "$MOCK_STATE/query"
case "$(cat "$MOCK_STATE/mode")" in
    missing)
        printf 'release not found\n' >&2
        exit 1
        ;;
    draft)
        printf 'true\n'
        ;;
    published)
        printf 'false\n'
        ;;
    error)
        printf 'network unavailable\n' >&2
        exit 1
        ;;
    *)
        exit 2
        ;;
esac
MOCK
chmod +x "$MOCK_BIN/gh"

run_prepare() {
    output=$1
    mode=$2
    printf '%s\n' "$mode" > "$MOCK_STATE/mode"
    (
        cd "$REPO"
        PATH="$MOCK_BIN:$PATH" MOCK_STATE="$MOCK_STATE" GH_TOKEN=test \
            GITHUB_ACTIONS=true GITHUB_REF=refs/heads/main \
            RELEASE_REPOSITORY=coffee-clang/cup GITHUB_OUTPUT="$output" \
            scripts/release/prepare.sh
    )
}

run_prepare "$TMP_ROOT/missing.out" missing >/dev/null
grep -Fx 'should_release=1' "$TMP_ROOT/missing.out" >/dev/null
run_prepare "$TMP_ROOT/draft.out" draft >/dev/null
grep -Fx 'should_release=1' "$TMP_ROOT/draft.out" >/dev/null
run_prepare "$TMP_ROOT/published.out" published >/dev/null
grep -Fx 'should_release=0' "$TMP_ROOT/published.out" >/dev/null
grep -F -- '--repo coffee-clang/cup' "$MOCK_STATE/query" >/dev/null

if run_prepare "$TMP_ROOT/error.out" error >"$TMP_ROOT/error.log" 2>&1; then
    fail 'public release query failure unexpectedly produced a candidate decision'
fi
grep -F 'could not query public release' "$TMP_ROOT/error.log" >/dev/null

(
    cd "$REPO"
    PATH="$MOCK_BIN:$PATH" MOCK_STATE="$MOCK_STATE" GH_TOKEN=test \
        GITHUB_ACTIONS=true GITHUB_REF=refs/heads/feature \
        RELEASE_REPOSITORY=coffee-clang/cup GITHUB_OUTPUT="$TMP_ROOT/branch.out" \
        scripts/release/prepare.sh
) >/dev/null
grep -Fx 'should_release=0' "$TMP_ROOT/branch.out" >/dev/null

printf 'Release candidate decision tests passed.\n'

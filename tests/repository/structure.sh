#!/bin/sh

# Purpose: Enforces current repository boundaries, file types and executable bits.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Repository structure test failed: %s\n' "$*" >&2
    exit 1
}

tracked_shell_scripts() {
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files -- '*.sh'
    else
        find scripts tests -type f -name '*.sh' -print
    fi
}

for required in \
    config/dependencies.lock \
    config/dependencies.recipe \
    .github/workflows/dependencies.yml \
    .github/workflows/tests.yml \
    .github/workflows/release.yml \
    .github/workflows/debug.yml \
    scripts/dependencies/common.sh \
    scripts/dependencies/sources.sh \
    scripts/dependencies/verify.sh \
    tests/runners/unit.sh \
    tests/runners/integration-posix.sh \
    tests/runners/repository.sh \
    tests/runners/coverage.sh \
    tests/runners/sanitizers.sh; do
    [ -f "$required" ] || fail "required file is missing: $required"
done

[ ! -e tests/README.md ] || fail 'test documentation belongs in docs/development/TESTING.md'

unexpected=$(find tests/unit -type f ! -name '*.c' ! -name '*.h' -print)
[ -z "$unexpected" ] || fail "unsupported unit-test files: $unexpected"
unexpected=$(find tests/helpers -type f ! -name '*.c' -print)
[ -z "$unexpected" ] || fail "unsupported test-helper files: $unexpected"
unexpected=$(find tests/repository tests/integration/posix tests/runners tests/build \
    -type f ! -name '*.sh' -print)
[ -z "$unexpected" ] || fail "non-shell file in a POSIX test directory: $unexpected"
unexpected=$(find tests/integration/windows -type f ! -name '*.ps1' -print)
[ -z "$unexpected" ] || fail "non-PowerShell Windows integration file: $unexpected"

unversioned_actions=$(find .github/workflows -type f -name '*.yml' \
    -exec grep -HnE 'uses:[[:space:]]+[^.]' {} + \
    | grep -Ev '@v[0-9]+([[:space:]]|$)' || :)
[ -z "$unversioned_actions" ] ||
    fail "external actions must use numeric major tags:\n$unversioned_actions"

nonportable_shell=$(
    tracked_shell_scripts | while IFS= read -r script; do
        [ "$script" = tests/repository/structure.sh ] && continue
        grep -HnE \
            '(readlink[[:space:]]+-f|stat[[:space:]]+(-c|--format)|date[[:space:]]+-d|xargs[[:space:]]+-r|sort[[:space:]]+-V|find.*[[:space:]]-printf)' \
            "$script" 2>/dev/null || :
    done
)
[ -z "$nonportable_shell" ] || fail "non-portable shell options remain:\n$nonportable_shell"

# The first line defines ownership: standalone shell programs have a shebang
# and mode 100755; sourced libraries have no shebang and mode 100644.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git ls-files -s -- '*.sh' | while read -r mode object stage path; do
        first=$(sed -n '1p' "$path")
        case "$first" in
            '#!'*) expected=100755 ;;
            *) expected=100644 ;;
        esac
        [ "$mode" = "$expected" ] ||
            fail "$path has Git mode $mode; expected $expected"
    done
fi

tracked_shell_scripts | while IFS= read -r script; do
    first=$(sed -n '1p' "$script")
    case "$first" in
        '#!'*) [ -x "$script" ] || fail "entry point is not executable: $script" ;;
        *) [ ! -x "$script" ] || fail "sourced library is executable: $script" ;;
    esac
done

tracked_outputs=$(git ls-files 2>/dev/null | grep -E '^(build|dist|book)/' || :)
[ -z "$tracked_outputs" ] || fail "generated output is tracked:\n$tracked_outputs"

printf '%s\n' 'Repository structure tests passed.'

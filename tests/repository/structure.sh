#!/bin/sh

# Verifies tracked-file hygiene and executable bits.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Repository structure test failed: %s\n' "$*" >&2
    exit 1
}

repository_shell_scripts() {
    find scripts tests -type f -name '*.sh' -print
}

# Dependency metadata is parsed identically on every runner. Pin its checkout
# representation to LF and reject carriage returns in the tracked files.
carriage_return=$(printf '\r')
for file in config/dependencies.lock; do
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        attribute=$(git check-attr eol -- "$file")
        [ "$attribute" = "$file: eol: lf" ] ||
            fail "$file must be checked out with LF line endings"
    fi
    if LC_ALL=C grep -q "$carriage_return" "$file"; then
        fail "$file contains carriage-return characters"
    fi
done

msys2_setup_count=$(grep -h 'uses: msys2/setup-msys2@' .github/workflows/*.yml | wc -l | tr -d ' ')
msys2_diffutils_count=$(grep -h '^[[:space:]]*diffutils[[:space:]]*$' .github/workflows/*.yml | wc -l | tr -d ' ')
[ "$msys2_setup_count" -eq "$msys2_diffutils_count" ] ||
    fail 'Every MSYS2 workflow environment must install diffutils for cmp.'

python_usage=$(grep -R -nE '(^|[^[:alnum:]_])(python3?|pyyaml)([^[:alnum:]_]|$)' \
    Makefile scripts tests .github 2>/dev/null \
    | grep -v '^tests/repository/structure.sh:' || :)
[ -z "$python_usage" ] ||
    fail "Python/PyYAML is not allowed in repository tooling:\n$python_usage"

registration_tmp=$(mktemp -d "${TMPDIR:-/tmp}/cup-structure-registration.XXXXXX") ||
    fail 'could not allocate registration-test workspace'
cleanup_registration() { rm -rf -- "$registration_tmp"; }
trap cleanup_registration EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Every repository contract must be registered by the canonical runner.
for repository_test in tests/repository/*.sh; do
    grep -Fq "$repository_test" tests/runners/repository.sh ||
        fail "repository test is not registered: $repository_test"
done

# Unit/helper sources must be consumed by their canonical build scripts.
for unit_source in tests/unit/test_*.c; do
    grep -Fq "$unit_source" tests/build/unit.sh ||
        fail "unit-test source is not registered: $unit_source"
done
for helper_source in tests/helpers/*.c; do
    case "$helper_source" in
        tests/helpers/coverage-entry.c) continue ;;
    esac
    grep -Fq "$helper_source" tests/build/helpers.sh ||
        fail "test-helper source is not registered: $helper_source"
done

# Every unique Unity test definition must be registered, and every RUN_TEST
# target must exist. Duplicate function names in independent suites are allowed.
grep -R -h -E \
    '^[[:space:]]*(static[[:space:]]+)?void[[:space:]]+test_[A-Za-z0-9_]+[[:space:]]*\(' \
    tests/unit/*.c |
    sed -E 's/^[[:space:]]*(static[[:space:]]+)?void[[:space:]]+(test_[A-Za-z0-9_]+).*/\2/' |
    LC_ALL=C sort -u > "$registration_tmp/defined-tests"
grep -R -h -oE 'RUN_TEST[[:space:]]*\([[:space:]]*test_[A-Za-z0-9_]+' \
    tests/unit/*.c |
    sed -E 's/.*\([[:space:]]*(test_[A-Za-z0-9_]+)/\1/' |
    LC_ALL=C sort -u > "$registration_tmp/run-tests"
unregistered=$(comm -23 "$registration_tmp/defined-tests" "$registration_tmp/run-tests")
[ -z "$unregistered" ] ||
    fail "Unity tests are defined but not registered:\n$unregistered"
undefined=$(comm -13 "$registration_tmp/defined-tests" "$registration_tmp/run-tests")
[ -z "$undefined" ] ||
    fail "RUN_TEST references undefined tests:\n$undefined"

# Generic file assertions must not follow a symlink to a regular file.
printf '%s\n' target > "$registration_tmp/assertion-target"
ln -s "$registration_tmp/assertion-target" "$registration_tmp/assertion-link"
if (
    TESTS_ROOT="$ROOT/tests"
    export TESTS_ROOT
    . "$ROOT/tests/support/common.sh"
    assert_file "$registration_tmp/assertion-link"
) >"$registration_tmp/assertion.out" 2>&1; then
    fail 'assert_file accepted a symlink to a regular file'
fi

# Build-dependent repository checks are run by the Linux x64 source-test plan.
for build_test in \
    tests/repository/certs.sh \
    tests/repository/build-paths.sh \
    tests/repository/reproducibility.sh; do
    grep -Fq "$build_test" scripts/ci/source-posix.sh ||
        fail "build-dependent repository test is not in the source CI plan: $build_test"
done
grep -Fq 'CUP_CI_BUILD_REPOSITORY_TESTS: 1' .github/workflows/tests.yml ||
    fail 'Tests workflow does not enable build-dependent repository checks'
repository_timeout=$(sed -n \
    's/^[[:space:]]*CUP_TEST_REPOSITORY_TIMEOUT:[[:space:]]*//p' \
    .github/workflows/tests.yml)
case "$repository_timeout" in
    ''|*[!0-9]*|0) fail 'Tests workflow does not use a positive repository timeout' ;;
esac
unit_timeout=$(sed -n \
    's/^[[:space:]]*CUP_TEST_UNIT_TIMEOUT:[[:space:]]*//p' \
    .github/workflows/tests.yml)
case "$unit_timeout" in
    ''|*[!0-9]*|0) fail 'Windows source tests do not use a positive unit-test timeout' ;;
esac

portability_catalog_writer=$(sed -n \
    '/^write_package_catalog() {/,/^}/p' tests/portability/linux-static-runtime.sh)
printf '%s\n' "$portability_catalog_writer" | grep -Fq 'format=1' ||
    fail 'portability package-catalog fixture does not declare format=1'

if CUP_TEST_REPOSITORY_TIMEOUT=invalid tests/runners/repository.sh \
        >"$registration_tmp/invalid-timeout.out" 2>&1; then
    fail 'repository runner accepted an invalid per-contract timeout'
fi
grep -Fq 'Invalid CUP_TEST_REPOSITORY_TIMEOUT' \
    "$registration_tmp/invalid-timeout.out" ||
    fail 'invalid repository timeout did not produce an actionable diagnostic'

for required_option in \
    'halt_on_error=1' \
    'UNIT_ASAN_OPTIONS="$ASAN_BASE_OPTIONS:detect_leaks=$LEAKS"' \
    'INTEGRATION_ASAN_OPTIONS="$ASAN_BASE_OPTIONS:detect_leaks=$LEAKS"' \
    'halt_on_error=1:print_stacktrace=1' \
    'exitcode=23'; do
    grep -Fq "$required_option" tests/runners/sanitizers.sh ||
        fail "sanitizer runner does not enforce: $required_option"
done

# The first line defines the script role: standalone shell programs have a shebang
# and mode 100755; sourced libraries have no shebang and mode 100644.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git ls-files -s -- '*.sh' | while read -r mode object stage path; do
        [ -f "$path" ] || continue
        first=$(sed -n '1p' "$path")
        case "$first" in
            '#!'*) expected=100755 ;;
            *) expected=100644 ;;
        esac
        [ "$mode" = "$expected" ] ||
            fail "$path has Git mode $mode; expected $expected"
    done
fi

repository_shell_scripts | while IFS= read -r script; do
    first=$(sed -n '1p' "$script")
    case "$first" in
        '#!'*) [ -x "$script" ] || fail "entry point is not executable: $script" ;;
        *) [ ! -x "$script" ] || fail "sourced library is executable: $script" ;;
    esac
done

tracked_outputs=$(git ls-files 2>/dev/null | grep -E '^(build|dist|book)/' || :)
[ -z "$tracked_outputs" ] || fail "generated output is tracked:\n$tracked_outputs"

trap - EXIT HUP INT TERM
cleanup_registration
printf '%s\n' 'Repository structure tests passed.'

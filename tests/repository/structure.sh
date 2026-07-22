#!/bin/sh

# Purpose: Enforces repository ownership boundaries, executable-bit rules,
# and required test and pipeline entry points.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Repository structure test failed: %s\n' "$*" >&2
    exit 1
}

legacy_path=scripts/tests
[ ! -e "$legacy_path" ] || fail "obsolete test tree still exists: $legacy_path"
[ ! -e AGENTS.md ] ||
    fail 'repository-specific LLM instructions must not be shipped as project documentation'
[ ! -e tests/README.md ] ||
    fail 'test documentation belongs in docs/development/TESTING.md'

for obsolete in \
    src/entrypoints.c \
    include/entrypoints.h \
    src/transaction.c \
    include/transaction.h \
    src/fetch.c \
    include/fetch.h \
    src/install_config.c \
    include/install_config.h \
    src/commands_install.c \
    src/commands_install_plan.c \
    src/commands_config.c \
    src/commands_remove.c \
    src/commands_state.c \
    src/commands_update.c \
    tests/integration/posix/entrypoints.sh \
    tests/integration/posix/commands.sh \
    tests/integration/windows/entrypoints.ps1 \
    .github/workflows/ci.yml \
    .github/workflows/source-tests.yml \
    .github/workflows/build-release.yml \
    .github/workflows/test-release.yml; do
    [ ! -e "$obsolete" ] || fail "obsolete repository path still exists: $obsolete"
done

stale=$(grep -RIl --exclude=structure.sh -- "$legacy_path" \
    Makefile README.md docs .github scripts tests 2>/dev/null || :)
[ -z "$stale" ] || fail "obsolete test-tree references remain in:
$stale"

stale_contracts=
for pattern in \
    'entrypoints.c' 'entrypoints.h' 'entrypoints.sh' 'entrypoints.ps1' \
    'deps-development' 'CUP_DEPS_SCOPE=development' \
    'source-tests.yml' 'build-release.yml' 'test-release.yml' 'ci.yml'; do
    matches=$(grep -RIl --exclude=structure.sh --exclude-dir=build \
        --exclude-dir=.git -- "$pattern" Makefile README.md docs .github scripts tests 2>/dev/null || :)
    [ -z "$matches" ] || stale_contracts="$stale_contracts $pattern:$matches"
done
[ -z "$stale_contracts" ] ||
    fail "obsolete module, dependency or workflow references remain:$stale_contracts"

retired_runtime=$(grep -RInE \
    '\.cup/(tmp|scripts)|\.cup\\scripts|layout_get_tmp|journal_version=2|operation=self-update' \
    include src scripts docs --include='*.c' --include='*.h' --include='*.sh' \
    --include='*.ps1' --include='*.md' 2>/dev/null || :)
[ -z "$retired_runtime" ] ||
    fail "retired runtime layout or journal contracts remain:
$retired_runtime"

unversioned_actions=$(find .github/workflows -type f -name '*.yml' \
    -exec grep -HnE 'uses:[[:space:]]+[^.]' {} + \
    | grep -Ev '@v[0-9]+([[:space:]]|$)' || :)
[ -z "$unversioned_actions" ] || \
    fail "external workflow actions must use explicit numeric major tags:
$unversioned_actions"

unexpected_c=$(find tests -type f -name '*.c' \
    ! -path 'tests/unit/*' ! -path 'tests/helpers/*' -print)
[ -z "$unexpected_c" ] ||
    fail "C test sources outside tests/unit or tests/helpers: $unexpected_c"

unexpected_helpers=$(find tests/helpers -type f ! -name '*.c' -print 2>/dev/null || :)
[ -z "$unexpected_helpers" ] ||
    fail "test helpers contain unsupported files: $unexpected_helpers"

unexpected_unit=$(find tests/unit -type f \
    ! -name '*.c' ! -name '*.h' -print)
[ -z "$unexpected_unit" ] || fail "non-C unit-test files found in tests/unit: $unexpected_unit"

unexpected_repository=$(find tests/repository -type f ! -name '*.sh' -print)
[ -z "$unexpected_repository" ] ||
    fail "repository tests must be POSIX shell scripts: $unexpected_repository"

unexpected_posix=$(find tests/integration/posix -type f ! -name '*.sh' -print)
[ -z "$unexpected_posix" ] ||
    fail "POSIX integration tests contain non-shell files: $unexpected_posix"

unexpected_windows=$(find tests/integration/windows -type f ! -name '*.ps1' -print)
[ -z "$unexpected_windows" ] ||
    fail "Windows integration tests contain non-PowerShell files: $unexpected_windows"

unexpected_runner=$(find tests/runners -type f ! -name '*.sh' -print)
[ -z "$unexpected_runner" ] ||
    fail "test runners contain unsupported files: $unexpected_runner"

unexpected_build=$(find tests/build -type f ! -name '*.sh' -print)
[ -z "$unexpected_build" ] ||
    fail "test build scripts contain unsupported files: $unexpected_build"

nonportable_shell=$(
    {
        grep -RInE --exclude=structure.sh --include='*.sh' \
            '(readlink[[:space:]]+-f|stat[[:space:]]+(-c|--format)|date[[:space:]]+-d)' \
            scripts tests 2>/dev/null || :
        grep -RInE --exclude=structure.sh --include='*.sh' \
            '(xargs[[:space:]]+-r|sort[[:space:]]+-V)' \
            scripts tests 2>/dev/null || :
        grep -RInE --exclude=structure.sh --include='*.sh' \
            'find.*[[:space:]]-printf' scripts tests 2>/dev/null || :
        grep -RInE --exclude=structure.sh --include='*.sh' \
            'sed.*[[:digit:]]+g' scripts tests 2>/dev/null || :
    } | sort -u
)
[ -z "$nonportable_shell" ] ||
    fail "non-portable shell utility options remain:
$nonportable_shell"

# Keep one authoritative executable-bit policy. The fixer is intentionally run
# through sh so it also works immediately after a ZIP extraction strips its own
# executable bit.
sh ./scripts/fix-shell-permissions.sh --check
for required in \
    .editorconfig \
    tests/runners/unit.sh \
    tests/runners/repository.sh \
    tests/runners/integration-posix.sh \
    tests/runners/coverage.sh \
    tests/runners/sanitizers.sh \
    tests/build/unit.sh \
    tests/build/helpers.sh \
    tests/repository/assertions.sh \
    tests/repository/coverage-policy.sh \
    tests/repository/dependencies.sh \
    tests/repository/filesystem-security.sh \
    tests/unit/test_exit_status.c \
    tests/unit/test_interrupt.c \
    tests/unit/test_package_install.c \
    tests/unit/test_command_install.c \
    tests/unit/test_command_config.c \
    tests/unit/test_command_update.c \
    tests/unit/test_command_queries.c \
    tests/unit/test_archive_faults.c \
    tests/unit/test_command_doctor.c \
    tests/unit/test_package_transaction.c \
    tests/unit/test_cup_update_journal.c \
    tests/unit/test_runtime_journal.c \
    tests/unit/test_wrappers.c \
    include/exit_status.h \
    include/package_transaction.h \
    include/cup_update_journal.h \
    include/cup_update_helper.h \
    include/runtime_journal.h \
    src/exit_status.c \
    src/package_transaction.c \
    src/cup_update_journal.c \
    src/cup_update_helper.c \
    src/runtime_journal.c \
    tests/unit/test_command_repair.c \
    tests/unit/test_package_extract.c \
    tests/integration/posix/cli-contract.sh \
    tests/integration/posix/package-lifecycle.sh \
    tests/integration/windows/recovery.ps1 \
    tests/integration/windows/filesystem-archives.ps1 \
    tests/portability/linux-network.sh \
    tests/helpers/archive-fixture.c \
    tests/helpers/network-helper.c \
    scripts/fix-shell-permissions.sh \
    scripts/build/inspect-binary.sh \
    scripts/build/validate-toolchain.sh \
    scripts/build/write-config.sh \
    scripts/ci/prepare-posix.sh \
    scripts/ci/source-posix.sh \
    scripts/ci/source-windows.ps1 \
    scripts/release/prepare.sh \
    scripts/release/decision-assets.sh \
    scripts/release/common-assets.sh \
    scripts/release/build-platform.sh \
    scripts/release/resolve-tests-run.sh \
    scripts/release/candidate-info.sh \
    scripts/release/publish.sh \
    .github/workflows/static.yml \
    .github/workflows/tests.yml \
    .github/workflows/release.yml; do
    [ -f "$required" ] || fail "required test or pipeline entry point is missing: $required"
done

grep -Fq 'root = true' .editorconfig ||
    fail '.editorconfig does not declare the repository root'
grep -Fq 'CUP_COVERAGE_MIN_FUNCTIONS' tests/runners/coverage.sh ||
    fail 'coverage runner does not gate function coverage'
grep -Fq 'CUP_TEST_SUITE_TIMEOUT' tests/runners/integration-posix.sh ||
    fail 'integration runner does not support bounded coverage suites'
if grep -Eq '\b(CC|gcc|clang)[[:space:]]' tests/runners/unit.sh; then
    fail 'unit runner compiles tests instead of only executing Makefile outputs'
fi
grep -Fq 'test-unit-build' Makefile ||
    fail 'Makefile does not own unit-test compilation'
grep -Fq 'test-helpers' Makefile ||
    fail 'Makefile does not own test-helper compilation'
grep -Fq 'pkg-config --static --libs libevent_extra libevent_core' \
        tests/build/helpers.sh ||
    fail 'network helper does not use the pinned libevent metadata'
production_libevent=$(grep -RInE 'event2/|(^|[[:space:]])-levent|libevent_(core|extra)' \
    include src scripts/build Makefile 2>/dev/null || :)
[ -z "$production_libevent" ] ||
    fail "libevent leaked into the CUP application build:
$production_libevent"


legacy_modules=$(find src -maxdepth 1 -type f -name 'commands_*.c' -print)
[ -z "$legacy_modules" ] || fail "plural command modules remain: $legacy_modules"

if grep -RInE '\bsystem_start_cup_update[[:space:]]*\(' \
        include/system.h src/system_posix.c src/system_windows.c >/dev/null 2>&1; then
    fail 'CUP update policy leaked back into the operating-system backend'
fi

if grep -Eq 'PackageRequest|installed_package_|runtime_journal_' \
        include/command_context.h; then
    fail 'command_context.h exposes request, package or journal policy'
fi

if grep -Fq 'package_install_update_scope' include/commands.h; then
    fail 'commands.h exposes an internal package-install operation'
fi

script_lang=pyth
script_lang=${script_lang}on
script_ext=.py
owned_scripts=$(find . -path './.git' -prune -o -path './build' -prune -o \
    -type f -name "*$script_ext" -print)
[ -z "$owned_scripts" ] || fail "repository-owned $script_lang files remain: $owned_scripts"
invocations=$(grep -RInE "(^|[^A-Za-z0-9_])${script_lang}(3)?([^A-Za-z0-9_]|$)" \
    Makefile README.md docs .github scripts tests 2>/dev/null || :)
[ -z "$invocations" ] || fail "$script_lang invocations remain:
$invocations"

printf 'Repository test and pipeline structure is coherent.\n'

for required in scripts/build/check-path-leaks.sh scripts/build/finalize-release.sh \
        scripts/certs/check-ca-bundle.sh certs/cacert.meta; do
    if [ ! -f "$ROOT/$required" ]; then
        echo "missing required file: $required" >&2
        exit 1
    fi
done

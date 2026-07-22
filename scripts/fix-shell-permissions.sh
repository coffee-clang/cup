#!/bin/sh

# Purpose: Restores the shell-file permissions expected by the CUP repository.
# Run this after extracting a ZIP archive, because ZIP tools may discard or
# broaden Unix executable bits. The same policy is used by repository tests.
set -eu

usage() {
    printf '%s\n' \
        'Usage: sh scripts/fix-shell-permissions.sh [--fix|--check]' \
        '' \
        '  --fix   restore working-tree and Git-index executable bits (default)' \
        '  --check verify the current permissions without changing files'
}

fail() {
    printf 'Shell permission check failed: %s\n' "$*" >&2
    exit 1
}

mode=${1:---fix}
case "$mode" in
    --fix|--check)
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if command -v git >/dev/null 2>&1 &&
    git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    root=$(git rev-parse --show-toplevel)
else
    root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fi
cd "$root"

# These files are sourced by another shell process. They are libraries, not
# command entry points, and therefore must not have an executable bit.
sourced_shell_files='scripts/dependencies/common.sh
scripts/dependencies/sources.sh
scripts/release/common.sh
tests/support/common.sh
tests/support/environment.sh
tests/support/posix-cli.sh
tests/support/quality-status.sh'

is_sourced_shell_file() {
    candidate=$1
    case "
$sourced_shell_files
" in
        *"
$candidate
"*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

tracked_shell_files() {
    if command -v git >/dev/null 2>&1 &&
        git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files -- 'scripts/*.sh' 'scripts/**/*.sh' \
            'tests/*.sh' 'tests/**/*.sh'
    else
        find scripts tests -type f -name '*.sh' | LC_ALL=C sort
    fi
}

index_mode_for() {
    git ls-files --stage -- "$1" | awk 'NR == 1 { print $1 }'
}

check_worktree_modes=1
case $(uname -s 2>/dev/null || printf 'unknown\n') in
    MSYS*|MINGW*|CYGWIN*)
        # MSYS executable-bit emulation depends on the checkout configuration.
        # The Git index remains authoritative on Windows.
        check_worktree_modes=0
        ;;
esac

missing_executable=
unexpected_executable=
wrong_index_mode=

while IFS= read -r file; do
    [ -n "$file" ] || continue
    [ -f "$file" ] || fail "tracked shell file is missing: $file"

    if is_sourced_shell_file "$file"; then
        expected_mode=100644
        if [ "$mode" = --fix ]; then
            chmod 0644 -- "$file"
            if command -v git >/dev/null 2>&1 &&
                git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
                git update-index --chmod=-x -- "$file"
            fi
        elif [ "$check_worktree_modes" -eq 1 ] && [ -x "$file" ]; then
            unexpected_executable="$unexpected_executable $file"
        fi
    else
        expected_mode=100755
        if [ "$mode" = --fix ]; then
            chmod 0755 -- "$file"
            if command -v git >/dev/null 2>&1 &&
                git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
                git update-index --chmod=+x -- "$file"
            fi
        elif [ "$check_worktree_modes" -eq 1 ] && [ ! -x "$file" ]; then
            missing_executable="$missing_executable $file"
        fi
    fi

    if command -v git >/dev/null 2>&1 &&
        git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        actual_mode=$(index_mode_for "$file")
        [ "$actual_mode" = "$expected_mode" ] ||
            wrong_index_mode="$wrong_index_mode $file($actual_mode->$expected_mode)"
    fi
done <<EOF_FILES
$(tracked_shell_files)
EOF_FILES

[ -z "$missing_executable" ] ||
    fail "shell entry point is not executable:$missing_executable"
[ -z "$unexpected_executable" ] ||
    fail "sourced shell library is executable:$unexpected_executable"
[ -z "$wrong_index_mode" ] ||
    fail "Git index contains incorrect shell modes:$wrong_index_mode"

if [ "$mode" = --fix ]; then
    # Re-run in check mode so a failed chmod or index update cannot be hidden.
    sh "$0" --check
    printf 'Shell permissions restored.\n'
else
    printf 'Shell permissions are correct.\n'
fi

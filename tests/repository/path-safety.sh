#!/bin/sh

# Proves the repository path layer prevents plausible path mistakes while keeping
# ordinary build, staging and publication usable.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"

test_begin path-safety
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"

assert_rejected() {
    if "$@" >"$TMP_ROOT/rejected.out" 2>&1; then
        fail "expected rejection: $*"
    fi
}

# Managed paths are canonical absolute shell paths.
assert_rejected cup_path_validate_absolute_clean relative 'relative path'
assert_rejected cup_path_validate_absolute_clean / 'filesystem root'
assert_rejected cup_path_validate_absolute_clean "$TMP_ROOT/" 'trailing slash'
assert_rejected cup_path_validate_absolute_clean "$TMP_ROOT/./child" 'dot component'
assert_rejected cup_path_validate_absolute_clean "$TMP_ROOT/../child" 'parent component'
assert_rejected cup_path_validate_absolute_clean "$TMP_ROOT/double//child" 'empty component'
assert_rejected cup_path_validate_absolute_clean "$TMP_ROOT/back\slash" 'backslash path'
assert_contains "$(cat "$TMP_ROOT/rejected.out")" 'must use forward slashes'

newline_path=$(printf '%s\n%s' "$TMP_ROOT/line" break)
carriage_path=$(printf '%s\r%s' "$TMP_ROOT/carriage" return)
assert_rejected cup_path_validate_absolute_clean "$newline_path" 'newline path'
assert_rejected cup_path_validate_absolute_clean "$carriage_path" 'carriage-return path'

# Host temporary spelling is normalized before entering the managed-path contract.
temporary_parent=$TMP_ROOT/temporary-parent
mkdir "$temporary_parent"
resolved_temporary=$(TMPDIR="$temporary_parent///" \
    cup_path_resolve_host_temporary_directory 'temporary parent')
assert_equals "$resolved_temporary" "$temporary_parent"

# Existing parent chains may not traverse symlinks or non-directories.
safe_parent=$TMP_ROOT/safe-parent
mkdir "$safe_parent"
external=$TMP_ROOT/external
mkdir "$external"
ln -s "$external" "$safe_parent/link"
assert_rejected cup_path_check_directory_chain "$safe_parent/link/child" 1 'linked path'
printf '%s\n' data > "$safe_parent/file"
assert_rejected cup_path_check_directory_chain "$safe_parent/file/child" 1 'file path'

# Normal directory preparation and containment work with spaces.
owned=$TMP_ROOT/'owned root'
cup_path_prepare_directory_chain "$owned/child dir" 'owned directory'
[ -d "$owned/child dir" ] || fail 'directory preparation failed'
cup_path_require_within "$owned" "$owned/child dir" 'owned child'
assert_rejected cup_path_require_within "$owned" "$TMP_ROOT/owned-root-sibling" 'sibling'

# Parallel build jobs may both observe a missing directory before one creates it.
# Simulate mkdir reporting failure after the directory has appeared; preparation
# must accept that benign outcome without accepting links or non-directories.
concurrent_parent=$TMP_ROOT/concurrent-parent
mkdir "$concurrent_parent"
(
    real_mkdir=$(command -v mkdir)
    mkdir() {
        "$real_mkdir" "$@"
        return 1
    }
    cup_path_prepare_directory_chain "$concurrent_parent/generated" 'concurrent directory'
)
[ -d "$concurrent_parent/generated" ] && [ ! -L "$concurrent_parent/generated" ] ||
    fail 'concurrent directory preparation did not preserve a real directory'

unique=$(cup_path_create_unique_directory "$owned/work.XXXXXX" 'unique directory' 0700)
case "$unique" in "$owned"/*) ;; *) fail 'unique directory escaped its parent' ;; esac
[ -d "$unique" ] && [ ! -L "$unique" ] || fail 'unique directory is invalid'

# File publication uses same-directory staging and preserves no-replace semantics.
target=$owned/output.txt
printf '%s\n' first | cup_path_write_file "$target" 0644 replace
assert_equals "$(cat "$target")" first
printf '%s\n' first | cup_path_write_file "$target" 0644 if-different
printf '%s\n' second | cup_path_write_file "$target" 0644 if-different
assert_equals "$(cat "$target")" second
if printf '%s\n' third | cup_path_write_file "$target" 0644 no-replace >/dev/null 2>&1; then
    fail 'no-replace publication overwrote an existing file'
fi
assert_equals "$(cat "$target")" second

source_file=$owned/source.bin
printf '%s\n' payload > "$source_file"
copied_file=$owned/copied.bin
cup_path_copy_file "$source_file" "$copied_file" 0755 replace
assert_equals "$(cat "$copied_file")" payload
[ -x "$copied_file" ] || fail 'copy-file did not apply requested executable mode'

# Existing output symlinks are rejected instead of being followed.
ln -s "$external" "$owned/symlink-output"
assert_rejected cup_path_prepare_child_file "$owned" "$owned/symlink-output" 'symlink output'

# Tree copies accept regular files/directories and reject links/special entries.
tree_source=$owned/tree-source
tree_destination=$owned/tree-destination
mkdir "$tree_source" "$tree_destination"
printf '%s\n' tree > "$tree_source/file"
cup_path_require_safe_tree "$tree_source" 'safe tree'
cup_path_copy_tree "$tree_source" "$tree_destination"
assert_equals "$(cat "$tree_destination/file")" tree
ln -s "$external" "$tree_source/link"
assert_rejected cup_path_require_safe_tree "$tree_source" 'linked tree'
rm "$tree_source/link"
mkfifo "$tree_source/fifo"
assert_rejected cup_path_require_safe_tree "$tree_source" 'special tree'
rm "$tree_source/fifo"

# Build roots are identified by one exact marker and reject dangerous roots.
build_root=$TMP_ROOT/build-root
cup_path_prepare_build_root "$build_root"
cup_path_require_build_root "$build_root"
assert_file "$build_root/.cup-build-root"
cup_path_prepare_build_root "$TMP_ROOT/unowned-build"
cup_path_require_build_root "$TMP_ROOT/unowned-build"
cup_path_clean_build_root "$TMP_ROOT/unowned-build"

bad_root=$TMP_ROOT/bad-build
mkdir "$bad_root"
printf '%s\n' foreign > "$bad_root/.cup-build-root"
assert_rejected cup_path_require_build_root "$bad_root"
assert_rejected cup_path_clean_build_root "$bad_root"
[ -d "$bad_root" ] || fail 'invalid build root was removed'
assert_rejected cup_path_prepare_build_root "${HOME%/}"

# An evident linked parent cannot redirect build-root creation.
linked_parent=$TMP_ROOT/linked-parent
ln -s "$external" "$linked_parent"
assert_rejected cup_path_prepare_build_root "$linked_parent/build"
assert_missing "$external/build"

# Owned build roots can be removed only after their marker is revalidated.
cup_path_clean_build_root "$build_root"
assert_missing "$build_root"

# Owned-child removal is confined and generic removals reject HOME/checkout.
remove_root=$TMP_ROOT/remove-root
mkdir "$remove_root"
mkdir "$remove_root/child"
printf '%s\n' keep > "$external/keep"
cup_path_remove_child_tree "$remove_root" "$remove_root/child" 'owned child'
assert_missing "$remove_root/child"
assert_rejected cup_path_remove_child_tree "$remove_root" "$external" 'external child'
assert_file "$external/keep"
assert_rejected cup_path_remove_directory_tree "${HOME%/}" 'HOME'
assert_rejected cup_path_remove_directory_tree "$PROJECT_ROOT" 'checkout'

# Move commits do not replace an already published destination.
move_source=$TMP_ROOT/move-source
move_destination=$TMP_ROOT/move-destination
printf '%s\n' source > "$move_source"
cup_path_move_entry "$move_source" "$move_destination"
assert_equals "$(cat "$move_destination")" source
printf '%s\n' replacement > "$move_source"
assert_rejected cup_path_move_entry "$move_source" "$move_destination"
assert_equals "$(cat "$move_destination")" source

case "$(uname -s 2>/dev/null || true)" in
    MSYS*|MINGW*|CYGWIN*)
        command -v cygpath >/dev/null 2>&1 || fail 'cygpath is required for Windows path tests'
        command -v cmd.exe >/dev/null 2>&1 || fail 'cmd.exe is required for Windows path tests'
        system_drive=$(MSYS2_ARG_CONV_EXCL='*' cmd.exe /d /c 'echo %SystemDrive%' | tr -d '\r' | sed -n '1p')
        [ -n "$system_drive" ] || fail 'could not resolve Windows system drive'
        drive_root=$(cygpath -u "$system_drive/")
        while [ "$drive_root" != / ]; do case "$drive_root" in */) drive_root=${drive_root%/} ;; *) break ;; esac; done
        assert_rejected cup_path_validate_absolute_clean "$drive_root" 'Windows drive root'
        ;;
esac

printf '%s\n' 'Repository path-safety tests passed.'

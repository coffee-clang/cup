#!/bin/sh

# Proves that managed filesystem operations remain confined to
# validated no-follow paths across deterministic path-replacement races.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"

file_mode() {
    if stat -c '%a' "$1" >/dev/null 2>&1; then
        stat -c '%a' "$1"
    else
        stat -f '%Lp' "$1"
    fi
}

test_begin path-safety

case "$(uname -s 2>/dev/null || true)" in
    MSYS*|MINGW*|CYGWIN*)
        printf '%s\n' 'Path-safety descriptor-race tests are POSIX-native only.'
        exit 0
        ;;
esac

command -v cc >/dev/null 2>&1 || fail 'cc is required for path-safety tests'
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"
if cup_path_validate_absolute_clean "$TMP_ROOT/back\slash" 'backslash test path' \
        >"$TMP_ROOT/backslash.out" 2>&1; then
    fail 'single backslash was accepted in a managed POSIX path'
fi
assert_contains "$(cat "$TMP_ROOT/backslash.out")" 'must use forward slashes'

helper=$TMP_ROOT/path-ops-testing
cc -std=c11 -O2 -Wall -Wextra -Werror -U_WIN32 \
    -DCUP_PATH_OPS_TESTING -DCUP_SYSTEM_TESTING \
    -I"$PROJECT_ROOT/include" \
    "$PROJECT_ROOT/scripts/lib/path-ops.c" \
    "$PROJECT_ROOT/src/system.c" \
    "$PROJECT_ROOT/src/system_posix.c" \
    "$PROJECT_ROOT/src/path.c" \
    "$PROJECT_ROOT/src/text.c" \
    -o "$helper"
chmod 0700 "$helper"

if CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" protocol \
        >"$TMP_ROOT/ambient-helper.out" 2>&1; then
    fail 'ambient filesystem-helper override was accepted outside repository tests'
fi
assert_contains "$(cat "$TMP_ROOT/ambient-helper.out")" 'reserved for repository tests'

if CUP_PATH_OPS_LAUNCHER="$PROJECT_ROOT/scripts/lib/path-ops.sh" \
        sh -c '. "$1"; cup_path_ops protocol' sh \
        "$PROJECT_ROOT/scripts/lib/path-safety.sh" \
        >"$TMP_ROOT/ambient-launcher.out" 2>&1; then
    fail 'ambient filesystem-helper launcher override was accepted outside repository tests'
fi
assert_contains "$(cat "$TMP_ROOT/ambient-launcher.out")" 'reserved for repository tests'

ambient_resolved=$TMP_ROOT/ambient-resolved-helper
cat > "$ambient_resolved" <<EOF_AMBIENT_RESOLVED
#!/bin/sh
: > '$TMP_ROOT/ambient-resolved-used'
printf '%s\n' 2
EOF_AMBIENT_RESOLVED
chmod 0700 "$ambient_resolved"
resolved_protocol=$(CUP_PATH_OPS_RESOLVED_HELPER="$ambient_resolved" sh -c \
    '. "$1"; cup_path_ops protocol' sh \
    "$PROJECT_ROOT/scripts/lib/path-safety.sh")
assert_equals "$resolved_protocol" 2
assert_missing "$TMP_ROOT/ambient-resolved-used"

# The MSYS launcher must select the native Windows filesystem backend while
# retaining the MSYS build-host compiler/process environment. A fake compiler
# makes the routing contract testable on POSIX hosts without claiming native
# Windows execution evidence.
fake_cc=$TMP_ROOT/fake-msys-cc
fake_log=$TMP_ROOT/fake-msys-cc.args
cat > "$fake_cc" <<'EOF_FAKE_CC'
#!/bin/sh
set -eu
if [ "${1:-}" = --version ]; then
    printf '%s\n' 'fake-msys-gcc 1.0'
    exit 0
fi
printf '%s\n' '-- invocation --' "$@" >> "$CUP_FAKE_CC_LOG"
out=
compile=0
while [ "$#" -gt 0 ]; do
    [ "$1" = -c ] && compile=1
    if [ "$1" = -o ]; then
        shift
        out=${1:-}
        break
    fi
    shift
done
[ -n "$out" ] || exit 2
if [ "$compile" -eq 1 ]; then
    : > "$out"
else
    cat > "$out" <<'EOF_FAKE_HELPER'
#!/bin/sh
[ "${1:-}" = protocol ] || exit 2
printf '%s\n' 2
EOF_FAKE_HELPER
fi
exit 0
EOF_FAKE_CC
chmod 0700 "$fake_cc"
windows_helper=$(OS=Windows_NT CUP_PATH_OPS_TESTING=1 \
    CUP_PATH_OPS_CC="$fake_cc" CUP_FAKE_CC_LOG="$fake_log" \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" --print-helper)
[ -x "$windows_helper" ] || fail 'simulated MSYS launcher did not publish its helper'
assert_contains "$(cat "$fake_log")" '-mwin32'
assert_contains "$(cat "$fake_log")" '-DCUP_PATH_OPS_MSYS_WINDOWS_BACKEND'
assert_contains "$(cat "$fake_log")" "$PROJECT_ROOT/src/system_windows.c"
assert_contains "$(cat "$fake_log")" '-ladvapi32'
if grep -F -- "$PROJECT_ROOT/src/system_posix.c" "$fake_log" >/dev/null; then
    fail 'simulated MSYS launcher selected the POSIX filesystem backend'
fi
if grep -F -- '-U_WIN32' "$fake_log" >/dev/null; then
    fail 'simulated MSYS launcher retained the POSIX-only feature mode'
fi
path_ops_invocation=$(awk '/-- invocation --/{block=""; next} {block=block $0 "\n"} /path-ops\.c/{print block; exit}' "$fake_log")
assert_contains "$path_ops_invocation" '-D_POSIX_C_SOURCE=200809L'
assert_not_contains "$path_ops_invocation" '-mwin32'
windows_invocation=$(awk '/-- invocation --/{block=""; next} {block=block $0 "\n"} /system_windows\.c/{print block; exit}' "$fake_log")
assert_contains "$windows_invocation" '-mwin32'
assert_contains "$windows_invocation" '-DCUP_PATH_OPS_MSYS_WINDOWS_BACKEND'

# A regular-file check must reject a symlink in any parent component, not only
# a symlink in the final component.
regular_external=$TMP_ROOT/regular-external
regular_root=$TMP_ROOT/regular-root
mkdir "$regular_external" "$regular_root"
printf '%s\n' data > "$regular_external/file"
ln -s "$regular_external" "$regular_root/link"
if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" check-file \
        "$regular_root/link/file" >"$TMP_ROOT/regular-parent.out" 2>&1; then
    fail 'check-file accepted a regular file through a symlink parent'
fi
assert_contains "$(cat "$TMP_ROOT/regular-parent.out")" 'not a no-follow regular file'

tree_source=$TMP_ROOT/tree-source
tree_destination=$TMP_ROOT/tree-destination
mkdir -p "$tree_source/bin" "$tree_destination"
printf '%s\n' executable >"$tree_source/bin/tool"
printf '%s\n' data >"$tree_source/data.txt"
chmod 0755 "$tree_source/bin/tool"
chmod 0644 "$tree_source/data.txt"
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" copy-tree \
    "$tree_source" "$tree_destination"
[ "$(file_mode "$tree_destination/bin/tool")" = 755 ] ||
    fail 'tree copy did not preserve normalized executable mode'
[ "$(file_mode "$tree_destination/data.txt")" = 644 ] ||
    fail 'tree copy did not preserve normalized data-file mode'

if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" copy-tree \
        "$tree_source" "$tree_source/nested" \
        >"$TMP_ROOT/overlap.out" 2>&1; then
    fail 'overlapping tree copy was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/overlap.out")" 'must not overlap'

mkdir -p "$TMP_ROOT/unique"
unique_one=$("$PROJECT_ROOT/scripts/lib/path-ops.sh" \
    mkdir-unique "$TMP_ROOT/unique/work.XXXXXX" 0700)
unique_two=$("$PROJECT_ROOT/scripts/lib/path-ops.sh" \
    mkdir-unique "$TMP_ROOT/unique/work.XXXXXX" 0700)
[ "$unique_one" != "$unique_two" ] || fail 'unique directory allocation reused a live name'
[ -d "$unique_one" ] && [ ! -L "$unique_one" ] || fail 'first unique directory is invalid'
[ -d "$unique_two" ] && [ ! -L "$unique_two" ] || fail 'second unique directory is invalid'
if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" mkdir-unique \
        "$TMP_ROOT/unique/noncanonical.XXXXXX" 700 \
        >"$TMP_ROOT/unique-mode.out" 2>&1; then
    fail 'mkdir-unique accepted a non-canonical mode'
fi
assert_contains "$(cat "$TMP_ROOT/unique-mode.out")" 'invalid file mode'

compiler_space=$TMP_ROOT/'compiler with spaces'
mkdir "$compiler_space"
ln -s "$(command -v cc)" "$compiler_space/cc"
compiler_protocol=$(CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_CC="$compiler_space/cc" \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" protocol)
assert_equals "$compiler_protocol" 2

wait_for_file() {
    path=$1
    process=$2
    attempt=0
    while [ ! -f "$path" ]; do
        if ! kill -0 "$process" 2>/dev/null; then
            wait "$process" || :
            fail "filesystem helper exited before publishing test pause: $path"
        fi
        attempt=$((attempt + 1))
        [ "$attempt" -lt 1000 ] || {
            kill "$process" 2>/dev/null || :
            wait "$process" 2>/dev/null || :
            fail "timed out waiting for filesystem helper pause: $path"
        }
        sleep 0.01
    done
}

run_path_operation() {
    point=$1
    ready=$2
    resume=$3
    operation=$4
    target=$5
    CUP_PATH_OPS_TESTING=1 \
    CUP_PATH_OPS_HELPER=$helper \
    CUP_PATH_OPS_TEST_POINT=$point \
    CUP_PATH_OPS_TEST_READY=$ready \
    CUP_PATH_OPS_TEST_CONTINUE=$resume \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" "$operation" "$target"
}

# Creation must remain relative to the already-opened original parent.
create_root=$TMP_ROOT/create
create_external=$TMP_ROOT/create-external
mkdir -p "$create_root/parent" "$create_external"
create_ready=$TMP_ROOT/create.ready
create_resume=$TMP_ROOT/create.resume
run_path_operation before-mkdir-component "$create_ready" "$create_resume" \
    ensure-dir "$create_root/parent/child" &
create_pid=$!
wait_for_file "$create_ready" "$create_pid"
mv "$create_root/parent" "$create_root/parent.original"
ln -s "$create_external" "$create_root/parent"
: > "$create_resume"
wait "$create_pid" || fail 'descriptor-relative directory creation failed'
[ -d "$create_root/parent.original/child" ] ||
    fail 'directory was not created under the opened original parent'
assert_missing "$create_external/child"
rm -f -- "$create_root/parent"
mv "$create_root/parent.original" "$create_root/parent"

# Unique allocation must re-open and validate the parent at the create point.
# If the checked parent is replaced by a symlink before creation, fail closed
# rather than allocating under the replacement target.
unique_race_root=$TMP_ROOT/unique-race
unique_race_external=$TMP_ROOT/unique-race-external
mkdir -p "$unique_race_root/parent" "$unique_race_external"
unique_race_ready=$TMP_ROOT/unique-race.ready
unique_race_resume=$TMP_ROOT/unique-race.resume
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=before-mkdir-unique \
CUP_PATH_OPS_TEST_READY=$unique_race_ready \
CUP_PATH_OPS_TEST_CONTINUE=$unique_race_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" mkdir-unique \
    "$unique_race_root/parent/work.XXXXXX" 0700 \
    >"$TMP_ROOT/unique-race.out" 2>&1 &
unique_race_pid=$!
wait_for_file "$unique_race_ready" "$unique_race_pid"
mv "$unique_race_root/parent" "$unique_race_root/parent.original"
ln -s "$unique_race_external" "$unique_race_root/parent"
: > "$unique_race_resume"
if wait "$unique_race_pid"; then
    fail 'unique directory creation followed a replaced parent'
fi
if find "$unique_race_external" -mindepth 1 -print -quit | grep -q .; then
    fail 'unique directory creation wrote into a replacement parent'
fi
if find "$unique_race_root/parent.original" -mindepth 1 -print -quit | grep -q .; then
    fail 'failed unique directory creation left an unexpected directory'
fi
rm -f -- "$unique_race_root/parent"
mv "$unique_race_root/parent.original" "$unique_race_root/parent"

# Shell preparation validates the final path itself before preparing parents.
if cup_path_prepare_file_target "$TMP_ROOT/unsafe-target/" 'unsafe output' \
        >"$TMP_ROOT/unsafe-target.out" 2>&1; then
    fail 'file-target preparation accepted a trailing slash'
fi
assert_contains "$(cat "$TMP_ROOT/unsafe-target.out")" 'must not have a trailing slash'
mkdir "$TMP_ROOT/owned-child-root"
if cup_path_prepare_child_file \
        "$TMP_ROOT/owned-child-root" "$TMP_ROOT/outside-child/file" 'child output' \
        >"$TMP_ROOT/outside-child.out" 2>&1; then
    fail 'child-file preparation accepted a target outside its owned root'
fi
assert_contains "$(cat "$TMP_ROOT/outside-child.out")" 'must stay inside'
assert_missing "$TMP_ROOT/outside-child"

# Removal must delete the original owned tree, never the replacement target.
remove_root=$TMP_ROOT/remove
remove_external=$TMP_ROOT/remove-external
mkdir -p "$remove_root/parent/tree" "$remove_external/tree"
printf '%s\n' owned > "$remove_root/parent/tree/owned.txt"
printf '%s\n' external > "$remove_external/tree/sentinel.txt"
remove_ready=$TMP_ROOT/remove.ready
remove_resume=$TMP_ROOT/remove.resume
run_path_operation before-remove-target "$remove_ready" "$remove_resume" \
    remove-tree "$remove_root/parent/tree" &
remove_pid=$!
wait_for_file "$remove_ready" "$remove_pid"
mv "$remove_root/parent" "$remove_root/parent.original"
ln -s "$remove_external" "$remove_root/parent"
: > "$remove_resume"
wait "$remove_pid" || fail 'descriptor-relative tree removal failed'
assert_missing "$remove_root/parent.original/tree"
assert_file "$remove_external/tree/sentinel.txt"
rm -f -- "$remove_root/parent"
mv "$remove_root/parent.original" "$remove_root/parent"


# Inherited private helper state must not bypass launcher resolution.
ambient_binary=$TMP_ROOT/ambient-path-ops-binary
cat > "$ambient_binary" <<EOF_AMBIENT_BINARY
#!/bin/sh
: > '$TMP_ROOT/ambient-binary-used'
exit 99
EOF_AMBIENT_BINARY
chmod 0700 "$ambient_binary"
protocol_output=$(_CUP_PATH_OPS_BINARY="$ambient_binary" sh -c \
    '. "$1"; cup_path_ops protocol' sh \
    "$PROJECT_ROOT/scripts/lib/path-safety.sh")
assert_equals "$protocol_output" 2
assert_missing "$TMP_ROOT/ambient-binary-used"

if CUP_PATH_OPS_CC=/bin/false "$PROJECT_ROOT/scripts/lib/path-ops.sh" protocol \
        >"$TMP_ROOT/ambient-compiler.out" 2>&1; then
    fail 'ambient filesystem-helper compiler override was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/ambient-compiler.out")" \
    'reserved for repository tests'

# Atomic no-replace publication preserves both existing inputs.
no_replace_root=$TMP_ROOT/no-replace
mkdir "$no_replace_root"
printf '%s\n' source > "$no_replace_root/source"
printf '%s\n' destination > "$no_replace_root/destination"
if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" move \
        "$no_replace_root/source" "$no_replace_root/destination" \
        >"$TMP_ROOT/no-replace-move.out" 2>&1; then
    fail 'move replaced an existing destination'
fi
assert_equals "$(cat "$no_replace_root/source")" source
assert_equals "$(cat "$no_replace_root/destination")" destination

if printf '%s\n' replacement | \
        CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" write-stdin \
        "$no_replace_root/destination" 0644 no-replace \
        >"$TMP_ROOT/no-replace-write.out" 2>&1; then
    fail 'no-replace write replaced an existing destination'
fi
assert_equals "$(cat "$no_replace_root/destination")" destination
if find "$no_replace_root" -maxdepth 1 -name '.cup-path-ops.*' -print -quit | grep -q .; then
    fail 'failed no-replace write left a temporary file'
fi

# A failure after the temporary file is created must still remove it.
mkdir "$no_replace_root/not-a-file"
if printf '%s\n' data | \
        CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" write-stdin \
        "$no_replace_root/not-a-file" 0644 \
        >"$TMP_ROOT/temp-cleanup.out" 2>&1; then
    fail 'write accepted a directory destination'
fi
if find "$no_replace_root" -maxdepth 1 -name '.cup-path-ops.*' -print -quit | grep -q .; then
    fail 'failed write left a temporary file'
fi

# Replacing the final file name after it was opened must be detected and the
# replacement must survive.
final_file_root=$TMP_ROOT/final-file
mkdir "$final_file_root"
printf '%s\n' owned > "$final_file_root/target"
final_file_ready=$TMP_ROOT/final-file.ready
final_file_resume=$TMP_ROOT/final-file.resume
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=before-remove-file-component \
CUP_PATH_OPS_TEST_READY=$final_file_ready \
CUP_PATH_OPS_TEST_CONTINUE=$final_file_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" remove-file \
    "$final_file_root/target" >"$TMP_ROOT/final-file.out" 2>&1 &
final_file_pid=$!
wait_for_file "$final_file_ready" "$final_file_pid"
mv "$final_file_root/target" "$final_file_root/original"
printf '%s\n' replacement > "$final_file_root/target"
: > "$final_file_resume"
if wait "$final_file_pid"; then
    fail 'file removal accepted a replaced final component'
fi
assert_equals "$(cat "$final_file_root/original")" owned
assert_equals "$(cat "$final_file_root/target")" replacement

# Replacing the source immediately before move must be rejected before
# publication rather than moving the replacement object.
move_race_root=$TMP_ROOT/move-race
mkdir "$move_race_root"
printf '%s\n' owned > "$move_race_root/source"
move_ready=$TMP_ROOT/move.ready
move_resume=$TMP_ROOT/move.resume
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=before-move-commit \
CUP_PATH_OPS_TEST_READY=$move_ready \
CUP_PATH_OPS_TEST_CONTINUE=$move_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" move \
    "$move_race_root/source" "$move_race_root/destination" \
    >"$TMP_ROOT/move-race.out" 2>&1 &
move_pid=$!
wait_for_file "$move_ready" "$move_pid"
mv "$move_race_root/source" "$move_race_root/original"
printf '%s\n' replacement > "$move_race_root/source"
: > "$move_resume"
if wait "$move_pid"; then
    fail 'move accepted a replaced source component'
fi
assert_equals "$(cat "$move_race_root/original")" owned
assert_equals "$(cat "$move_race_root/source")" replacement
assert_missing "$move_race_root/destination"
assert_contains "$(cat "$TMP_ROOT/move-race.out")" 'changed before publication'

# Recursive operations reject a filesystem boundary. The test-only boundary
# hook provides deterministic coverage without requiring mount privileges.
boundary_root=$TMP_ROOT/boundary
boundary_copy=$TMP_ROOT/boundary-copy
mkdir -p "$boundary_root/external" "$boundary_copy"
printf '%s\n' sentinel > "$boundary_root/external/sentinel"
for operation in check-tree remove-tree; do
    if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
            CUP_PATH_OPS_TEST_BOUNDARY_NAME=external \
            "$PROJECT_ROOT/scripts/lib/path-ops.sh" "$operation" "$boundary_root" \
            >"$TMP_ROOT/boundary-$operation.out" 2>&1; then
        fail "$operation crossed a forced filesystem boundary"
    fi
done
if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        CUP_PATH_OPS_TEST_BOUNDARY_NAME=external \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" copy-tree \
        "$boundary_root" "$boundary_copy" \
        >"$TMP_ROOT/boundary-copy.out" 2>&1; then
    fail 'copy-tree crossed a forced filesystem boundary'
fi
assert_file "$boundary_root/external/sentinel"

# A file mount is a boundary too. Force that shape separately so the recursive
# walk cannot accidentally apply the device check only to directory entries.
printf '%s\n' file-boundary > "$boundary_root/external-file"
for operation in check-tree copy-tree; do
    if [ "$operation" = copy-tree ]; then
        set -- "$boundary_root" "$boundary_copy"
    else
        set -- "$boundary_root"
    fi
    if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
            CUP_PATH_OPS_TEST_BOUNDARY_NAME=external-file \
            "$PROJECT_ROOT/scripts/lib/path-ops.sh" "$operation" "$@" \
            >"$TMP_ROOT/boundary-file-$operation.out" 2>&1; then
        fail "$operation crossed a forced regular-file filesystem boundary"
    fi
done
assert_file "$boundary_root/external-file"

# Build and clean share the canonical build-root marker lock. A concurrent
# clean must fail without modifying the active root.
build_lock_root=$TMP_ROOT/build-lock
mkdir "$build_lock_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$build_lock_root/.cup-build-root"
printf '%s\n' preserved > "$build_lock_root/sentinel"
build_lock_ready=$TMP_ROOT/build-lock.ready
build_lock_resume=$TMP_ROOT/build-lock.resume
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=after-build-lock \
CUP_PATH_OPS_TEST_READY=$build_lock_ready \
CUP_PATH_OPS_TEST_CONTINUE=$build_lock_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" run-build \
    "$build_lock_root" -- true >"$TMP_ROOT/build-lock.out" 2>&1 &
build_lock_pid=$!
wait_for_file "$build_lock_ready" "$build_lock_pid"
if CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" clean-build-root \
        "$build_lock_root" >"$TMP_ROOT/build-clean-busy.out" 2>&1; then
    fail 'clean acquired an active build root'
fi
assert_contains "$(cat "$TMP_ROOT/build-clean-busy.out")" 'build root is busy'
assert_file "$build_lock_root/sentinel"
: > "$build_lock_resume"
wait "$build_lock_pid" || fail 'locked build command failed'
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" clean-build-root "$build_lock_root"
assert_missing "$build_lock_root"

# A root replacement after the marker is locked must be rejected before the
# requested command is started, not merely noticed after the child exits.
build_swap_root=$TMP_ROOT/build-swap
mkdir "$build_swap_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$build_swap_root/.cup-build-root"
build_swap_ready=$TMP_ROOT/build-swap.ready
build_swap_resume=$TMP_ROOT/build-swap.resume
build_swap_command=$TMP_ROOT/build-swap-command-ran
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=before-build-lock-identity-check \
CUP_PATH_OPS_TEST_READY=$build_swap_ready \
CUP_PATH_OPS_TEST_CONTINUE=$build_swap_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" run-build "$build_swap_root" -- \
    sh -c ': > "$1"' sh "$build_swap_command" \
    >"$TMP_ROOT/build-swap.out" 2>&1 &
build_swap_pid=$!
wait_for_file "$build_swap_ready" "$build_swap_pid"
mv "$build_swap_root" "$build_swap_root.original"
mkdir "$build_swap_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$build_swap_root/.cup-build-root"
: > "$build_swap_resume"
if wait "$build_swap_pid"; then
    fail 'locked build accepted a replaced root before command execution'
fi
assert_missing "$build_swap_command"
assert_contains "$(cat "$TMP_ROOT/build-swap.out")" 'changed while acquiring lock'

# Replacing the root after lock acquisition but before child creation must also
# fail before the requested command is executed.
build_exec_root=$TMP_ROOT/build-exec-swap
mkdir "$build_exec_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$build_exec_root/.cup-build-root"
build_exec_ready=$TMP_ROOT/build-exec-swap.ready
build_exec_resume=$TMP_ROOT/build-exec-swap.resume
build_exec_command=$TMP_ROOT/build-exec-swap-command-ran
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=after-build-lock \
CUP_PATH_OPS_TEST_READY=$build_exec_ready \
CUP_PATH_OPS_TEST_CONTINUE=$build_exec_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" run-build "$build_exec_root" -- \
    sh -c ': > "$1"' sh "$build_exec_command" \
    >"$TMP_ROOT/build-exec-swap.out" 2>&1 &
build_exec_pid=$!
wait_for_file "$build_exec_ready" "$build_exec_pid"
mv "$build_exec_root" "$build_exec_root.original"
mkdir "$build_exec_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$build_exec_root/.cup-build-root"
: > "$build_exec_resume"
if wait "$build_exec_pid"; then
    fail 'locked build executed after its root was replaced'
fi
assert_missing "$build_exec_command"
assert_contains "$(cat "$TMP_ROOT/build-exec-swap.out")" 'changed while locked'

# The same locked identity must be checked before destructive clean starts.
clean_swap_root=$TMP_ROOT/clean-swap
mkdir "$clean_swap_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$clean_swap_root/.cup-build-root"
printf '%s\n' original > "$clean_swap_root/original.txt"
clean_swap_ready=$TMP_ROOT/clean-swap.ready
clean_swap_resume=$TMP_ROOT/clean-swap.resume
CUP_PATH_OPS_TESTING=1 CUP_PATH_OPS_HELPER=$helper \
CUP_PATH_OPS_TEST_POINT=after-clean-lock \
CUP_PATH_OPS_TEST_READY=$clean_swap_ready \
CUP_PATH_OPS_TEST_CONTINUE=$clean_swap_resume \
    "$PROJECT_ROOT/scripts/lib/path-ops.sh" clean-build-root "$clean_swap_root" \
    >"$TMP_ROOT/clean-swap.out" 2>&1 &
clean_swap_pid=$!
wait_for_file "$clean_swap_ready" "$clean_swap_pid"
mv "$clean_swap_root" "$clean_swap_root.original"
mkdir "$clean_swap_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$clean_swap_root/.cup-build-root"
printf '%s\n' replacement > "$clean_swap_root/replacement.txt"
: > "$clean_swap_resume"
if wait "$clean_swap_pid"; then
    fail 'build-root clean accepted a replaced root'
fi
assert_file "$clean_swap_root.original/original.txt"
assert_file "$clean_swap_root/replacement.txt"
assert_contains "$(cat "$TMP_ROOT/clean-swap.out")" 'changed while locked'

printf '%s\n' 'Descriptor-relative path-safety race tests passed.'

#!/usr/bin/env sh

# Produces one complete debug or release binary bundle without
# modifying the compiler output. Metadata, path-leak checks and binary
# inspection are completed inside staging before the output root is replaced.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$SCRIPT_DIR/../lib/path-safety.sh"

platform=${1:?platform is required}
mode=${2:?mode is required}
input=${3:?input binary is required}
output_root=${4:?output root is required}
build_config=${5:?build configuration metadata is required}
release_metadata=${6:?release metadata is required}
inspection_policy=${7:?inspection policy is required}
inspector=${8:?binary inspector is required}
path_leak_checker=${9:?path leak checker is required}
shift 9
build_root=${CUP_BUILD_ROOT:?CUP_BUILD_ROOT is required}

fail() {
    printf 'binary finalization: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found"
}

[ "$mode" = debug ] || [ "$mode" = release ] || fail "unsupported mode '$mode'"
[ "$inspection_policy" = build ] || [ "$inspection_policy" = public ] ||
    fail "unsupported inspection policy '$inspection_policy'"
for absolute_path in "$input" "$output_root" "$build_config" "$release_metadata" \
        "$inspector" "$path_leak_checker"; do
    case "$absolute_path" in
        /*|[A-Za-z]:/*) ;;
        *) fail "finalization path is not absolute: $absolute_path" ;;
    esac
done
cup_path_require_build_root "$build_root" || fail "invalid build root: $build_root"
cup_path_require_within "$build_root" "$input" "input binary" ||
    fail "input is outside the managed build root: $input"
cup_path_require_regular_file "$input" "input binary" ||
    fail "input is not a regular no-follow file: $input"
[ -s "$input" ] || fail "input is empty: $input"
[ -x "$input" ] || fail "input is not executable: $input"
for metadata in "$build_config" "$release_metadata"; do
    cup_path_require_within "$build_root" "$metadata" 'bundle metadata' ||
        fail "metadata is outside the managed build root: $metadata"
    cup_path_require_regular_file "$metadata" 'bundle metadata' ||
        fail "metadata is not a regular no-follow file: $metadata"
    [ -s "$metadata" ] || fail "metadata is empty: $metadata"
done
for tool_path in "$inspector" "$path_leak_checker"; do
    cup_path_require_regular_file "$tool_path" 'finalization tool' ||
        fail "finalization tool is not a regular no-follow file: $tool_path"
    [ -x "$tool_path" ] || fail "finalization tool is not executable: $tool_path"
done
cup_path_require_within "$build_root" "$output_root" "output root" ||
    fail "output root is outside the managed build root: $output_root"
[ "$input" != "$output_root" ] || fail 'input and output must be different'

parent=$(dirname "$output_root")
name=$(basename "$output_root")
staging=$parent/.$name.staging

cup_path_prepare_child_directory "$build_root" "$parent" "output parent" ||
    fail "could not prepare output parent: $parent"
cup_path_require_within "$build_root" "$staging" "staging path" || exit 1

# Recover only paths reserved for this derived output.
if [ -e "$staging" ] || [ -L "$staging" ]; then
    cup_path_check_directory_chain "$staging" 0 "staging path" ||
        fail "invalid staging path: $staging"
    cup_path_remove_child_tree "$build_root" "$staging" 'finalization staging' || exit 1
fi

cup_path_prepare_child_directory "$build_root" "$staging" "staging directory" || exit 1
cup_path_prepare_child_directory "$build_root" "$staging/bin" "staging binary directory" || exit 1
cup_path_prepare_child_directory "$build_root" "$staging/symbols" "staging symbols directory" || exit 1
case "$platform" in
    windows-x64) executable=$staging/bin/cup.exe ;;
    linux-*|macos-*) executable=$staging/bin/cup ;;
    *) fail "unsupported platform '$platform'" ;;
esac
cup_path_copy_file "$input" "$executable" 0755 replace ||
    fail "could not copy input binary into staging"

cleanup() {
    if [ -e "$staging" ] || [ -L "$staging" ]; then
        cup_path_check_directory_chain "$staging" 0 "staging path" >/dev/null 2>&1 || return 0
        cup_path_remove_child_tree "$build_root" "$staging" 'finalization staging' || return 0
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case "$platform" in
    linux-*)
        require_tool objcopy
        require_tool objdump
        require_tool strip
        require_tool readelf
        symbols=$staging/symbols/cup.debug
        objcopy --only-keep-debug "$input" "$symbols"
        chmod 0644 "$symbols"
        # Debug-only ELF files can retain unusable PT_INTERP metadata; inspect only sections here.
        objdump -h "$symbols" | grep -Eq '\.(debug_info|zdebug_info)|\.symtab' ||
            fail 'Linux symbol sidecar contains no usable debug information'
        if [ "$mode" = release ]; then
            strip --strip-unneeded "$executable"
            objcopy --add-gnu-debuglink="$symbols" "$executable"
            ! readelf -S "$executable" | grep -Eq '\.(debug_info|zdebug_info)' ||
                fail 'public Linux executable still contains debug information'
        else
            readelf -S "$executable" | grep -Eq '\.(debug_info|zdebug_info)' ||
                fail 'debug Linux executable contains no debug information'
        fi
        ;;
    macos-*)
        require_tool dsymutil
        require_tool dwarfdump
        require_tool strip
        symbols=$staging/symbols/cup.dSYM
        dsymutil "$input" -o "$symbols"
        dwarfdump --verify "$symbols" >/dev/null
        input_uuid=$(dwarfdump --uuid "$input" | awk '{ print $2 ":" $3 }' | sort)
        symbol_uuid=$(dwarfdump --uuid "$symbols" | awk '{ print $2 ":" $3 }' | sort)
        [ -n "$input_uuid" ] && [ "$input_uuid" = "$symbol_uuid" ] ||
            fail 'dSYM UUID does not match the executable'
        if [ "$mode" = release ]; then
            strip -S "$executable"
        fi
        ;;
    windows-x64)
        require_tool objcopy
        require_tool strip
        require_tool objdump
        symbols=$staging/symbols/cup.debug
        objcopy --only-keep-debug "$input" "$symbols"
        chmod 0644 "$symbols"
        objdump -h "$symbols" | grep -Eq '\.debug_|\.symtab' ||
            fail 'Windows symbol sidecar contains no usable debug information'
        if [ "$mode" = release ]; then
            strip --strip-unneeded "$executable"
        fi
        ;;
esac

[ -s "$executable" ] && [ -x "$executable" ] || fail 'final executable is invalid'
cup_path_copy_file "$build_config" "$staging/build-config.txt" 0644 replace ||
    fail 'could not stage build configuration metadata'
cup_path_copy_file "$release_metadata" "$staging/release.txt" 0644 replace ||
    fail 'could not stage release metadata'

if [ "$mode" = release ]; then
    "$path_leak_checker" "$executable" "$@" ||
        fail 'release path-leak inspection failed'
fi
CUP_BUILD_ROOT="$build_root" "$inspector" "$platform" "$mode" \
    "$executable" "$staging/binary-inspection.txt" "$inspection_policy" ||
    fail 'binary inspection failed'
cup_path_require_regular_file "$staging/binary-inspection.txt" 'binary inspection report' ||
    fail 'binary inspector did not produce a regular report'
[ -s "$staging/binary-inspection.txt" ] || fail 'binary inspection report is empty'

printf 'format=1\nplatform=%s\nmode=%s\n' "$platform" "$mode" |
    cup_path_write_file "$staging/finalization.txt" 0644 replace ||
    fail 'could not write finalization metadata'

if [ -e "$output_root" ] || [ -L "$output_root" ]; then
    cup_path_check_directory_chain "$output_root" 0 "existing output" ||
        fail "existing output is not a safe directory"
    cup_path_remove_child_tree "$build_root" "$output_root" 'existing finalized output' ||
        fail 'could not remove previous finalized output'
fi
cup_path_move_entry "$staging" "$output_root" || fail 'could not commit finalized output'
trap - EXIT HUP INT TERM
printf 'Finalized %s bundle: %s\n' "$mode" "$output_root"

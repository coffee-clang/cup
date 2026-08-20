#!/bin/sh

# Merges verified flat release parts without following links or hiding collisions.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
. "$SCRIPT_DIR/common.sh"

[ "$#" -ge 2 ] || {
    printf 'Usage: %s <output-directory> <part-directory>...\n' "$0" >&2
    exit 2
}

output=$1
shift
cup_path_validate_absolute_clean "$output" "candidate output" || exit 1
[ ! -e "$output" ] && [ ! -L "$output" ] ||
    fail "candidate output already exists: $output"
parent=$(dirname -- "$output")
cup_path_prepare_directory_chain "$parent" "candidate output parent" || exit 1
staging=$(cup_path_create_unique_directory \
    "$parent/.cup-candidate.XXXXXX" "candidate staging" 0755) || exit 1
cup_path_check_directory_chain "$staging" 0 "candidate staging" || exit 1
cleanup_assembly() {
    [ -z "${staging:-}" ] || cup_path_remove_directory_tree "$staging" 'candidate staging'
}
trap cleanup_assembly EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for part in "$@"; do
    require_real_directory "$part"
    names=$(mktemp "${TMPDIR:-/tmp}/cup-release-part.XXXXXX")
    for source in "$part"/* "$part"/.[!.]* "$part"/..?*; do
        [ -e "$source" ] || [ -L "$source" ] || continue
        [ -f "$source" ] && [ ! -L "$source" ] || {
            rm -f -- "$names"
            fail "release part contains a non-regular entry: $source"
        }
        basename -- "$source" >> "$names"
    done
    LC_ALL=C sort -o "$names" "$names"
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in .|..|*/*|*\\*)
            rm -f -- "$names"
            fail "unsafe release asset name: $name"
            ;;
        esac
        source=$part/$name
        require_nonempty_file "$source"
        [ ! -e "$staging/$name" ] && [ ! -L "$staging/$name" ] || {
            rm -f -- "$names"
            fail "duplicate release asset: $name"
        }
        mode=$(release_asset_mode "$name")
        cup_path_copy_file "$source" "$staging/$name" "$mode" replace ||
            fail "could not copy release asset: $source"
    done < "$names"
    rm -f -- "$names"
done

assembled_assets=$(
    for assembled in "$staging"/*; do basename -- "$assembled"; done | LC_ALL=C sort
)
# shellcheck disable=SC2086
validate_release_asset_modes "$staging" $assembled_assets
chmod 0755 "$staging"
cup_path_move_entry "$staging" "$output" || fail "could not commit candidate output"
staging=
trap - EXIT HUP INT TERM
printf '%s\n' "$output"

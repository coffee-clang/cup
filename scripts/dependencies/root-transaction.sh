# Owns dependency-root validation, locking, recovery and cleanup.
# Sourced by common.sh; not executable.

# Dependency prefix compatibility and transactional commit state.
# The root marker proves filesystem ownership; the prefix metadata records one
# compiled generation. Build, recovery and clean share the same adjacent lock.
CUP_DEPS_PREFIX_READY=0
CUP_DEPS_FINAL_PREFIX=
CUP_DEPS_STAGE_ROOT=
CUP_DEPS_BUILD_PREFIX=
CUP_DEPS_USE_OPENSSL=1
CUP_DEPS_BUILD_LOCK=

# Stable root ownership metadata. It intentionally excludes platform and build
# inputs because those belong to install/.cup-dependencies.
dependency_root_metadata() {
    printf '%s\n' \
        'format=1' \
        'product=coffee-clang/cup' \
        'kind=dependency-root' \
        'layout=1'
}

dependency_stream_matches_file() {
    local file="$1" temporary status=0
    temporary=$(mktemp "${TMPDIR:-/tmp}/cup-dependency-exact.XXXXXX") || return 1
    cat > "$temporary" || status=1
    if [ "$status" -eq 0 ] && ! cmp -s "$temporary" "$file"; then
        status=1
    fi
    rm -f -- "$temporary"
    return "$status"
}

dependency_read_canonical_owner() {
    local file="$1" owner
    owner=$(sed -n '1p' "$file") || return 1
    printf '%s\n' "$owner" | dependency_stream_matches_file "$file" || return 1
    printf '%s\n' "$owner"
}


dependency_validate_root_path() {
    local root="$1"

    dependency_require_whitespace_free_path "dependency root" "$root" || return 1
    cup_path_validate_absolute_clean "$root" "dependency root" || return 1
    if [ -n "${HOME:-}" ] && [ "$root" = "${HOME%/}" ]; then
        echo "Error: dependency root must not be the user home directory: $root" >&2
        return 1
    fi
    case "$CUP_PROJECT_ROOT/" in
        "$root/"*)
            echo "Error: dependency root must not contain the cup checkout: $root" >&2
            return 1
            ;;
    esac
}

dependency_lock_path() {
    local root="$1"
    local parent name
    parent=$(dirname "$root")
    name=$(basename "$root")
    printf '%s/.%s.cup-dependencies.lock\n' "$parent" "$name"
}

# Prevent builders and cleanup from mutating one managed dependency root
# concurrently. The lock is adjacent to the root so it survives deps-clean.
dependency_acquire_build_lock() {
    local root="$1"
    local lock parent owner owner_file entry temporary

    dependency_validate_root_path "$root" || return 1
    parent=$(dirname "$root")
    cup_path_prepare_directory_chain "$parent" "dependency root parent" || return 1
    lock=$(dependency_lock_path "$root")
    cup_path_validate_absolute_clean "$lock" "dependency lock" || return 1

    if ! cup_path_create_directory_exclusive "$lock" "dependency lock" 2>/dev/null; then
        cup_path_check_directory_chain "$lock" 0 "dependency lock" || return 1
        owner_file="$lock/owner"
        if [ ! -f "$owner_file" ] || [ -L "$owner_file" ]; then
            echo "Error: dependency lock is ambiguous and was preserved: $lock" >&2
            return 1
        fi
        owner=$(dependency_read_canonical_owner "$owner_file" 2>/dev/null || true)
        case "$owner" in
            ''|*[!0-9]*|0|0*)
                echo "Error: dependency lock has an invalid owner and was preserved: $lock" >&2
                return 1
                ;;
        esac
        if kill -0 "$owner" 2>/dev/null; then
            echo "Error: another dependency operation is active for $root (PID $owner)." >&2
            return 1
        fi
        for entry in "$lock"/* "$lock"/.[!.]* "$lock"/..?*; do
            [ -e "$entry" ] || [ -L "$entry" ] || continue
            [ "$entry" = "$owner_file" ] || {
                echo "Error: stale dependency lock contains unexpected data and was preserved: $lock" >&2
                return 1
            }
        done
        cup_path_remove_file "$owner_file" "stale dependency lock owner" || return 1
        cup_path_remove_empty_directory "$lock" "stale dependency lock" || return 1
        cup_path_create_directory_exclusive "$lock" "dependency lock" || {
            echo "Error: could not acquire dependency lock: $lock" >&2
            return 1
        }
    fi

    if ! printf '%s\n' "$$" | cup_path_write_file "$lock/owner" 0644 replace; then
        cup_path_remove_empty_directory "$lock" 'dependency lock' >/dev/null 2>&1 || true
        echo "Error: could not record dependency lock owner: $lock" >&2
        return 1
    fi
    CUP_DEPS_BUILD_LOCK=$lock
}

dependency_release_build_lock() {
    local owner=
    local owner_file

    [ -n "$CUP_DEPS_BUILD_LOCK" ] || return 0
    owner_file="$CUP_DEPS_BUILD_LOCK/owner"
    if [ -f "$owner_file" ] && [ ! -L "$owner_file" ]; then
        owner=$(dependency_read_canonical_owner "$owner_file" 2>/dev/null || true)
    fi
    if [ "$owner" = "$$" ]; then
        cup_path_remove_file "$owner_file" "dependency lock owner" || {
            CUP_DEPS_BUILD_LOCK=
            return 1
        }
        cup_path_remove_empty_directory "$CUP_DEPS_BUILD_LOCK" "dependency lock" || {
            echo "Error: dependency lock contains unexpected data and was preserved: $CUP_DEPS_BUILD_LOCK" >&2
            CUP_DEPS_BUILD_LOCK=
            return 1
        }
    fi
    CUP_DEPS_BUILD_LOCK=
}

dependency_root_marker_valid() {
    local root="$1"
    local marker="$root/$CUP_DEPENDENCY_ROOT_MARKER"
    [ -d "$root" ] && [ ! -L "$root" ] && [ -f "$marker" ] && [ ! -L "$marker" ] &&
        dependency_root_metadata | dependency_stream_matches_file "$marker"
}

dependency_write_root_marker() {
    local root="$1"
    local marker="$root/$CUP_DEPENDENCY_ROOT_MARKER"
    local temporary

    cup_path_check_directory_chain "$root" 0 "dependency root" || return 1
    dependency_root_metadata | cup_path_write_file "$marker" 0644 replace
}

dependency_directory_empty() {
    local directory="$1"
    local entry
    for entry in "$directory"/* "$directory"/.[!.]* "$directory"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        return 1
    done
    return 0
}

dependency_prefix_owned() {
    local prefix="$1"
    local marker="$prefix/.cup-dependencies"
    local metadata

    cup_path_check_directory_chain "$prefix" 0 "dependency prefix" >/dev/null 2>&1 || return 1
    cup_path_require_regular_file "$marker" "dependency prefix marker" >/dev/null 2>&1 || return 1
    metadata=$(cat "$marker") || return 1
    dependency_metadata_valid "$metadata" &&
        printf '%s\n' "$metadata" | dependency_stream_matches_file "$marker"
}

dependency_root_entries_are_adoptable() {
    local root="$1"
    local entry name
    for entry in "$root"/* "$root"/.[!.]* "$root"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        name=${entry##*/}
        case "$name" in
            src|build|install) ;;
            *)
                echo "Error: dependency root contains an unknown entry and cannot be adopted: $entry" >&2
                return 1
                ;;
        esac
        [ ! -L "$entry" ] || {
            echo "Error: dependency root entry is a symlink: $entry" >&2
            return 1
        }
    done
}

dependency_recover_root() {
    local root="$1"
    local staging="$root/.install.staging"

    if [ -e "$staging" ] || [ -L "$staging" ]; then
        [ -d "$staging" ] && [ ! -L "$staging" ] || {
            echo "Error: dependency staging path is not a managed directory: $staging" >&2
            return 1
        }
        if ! dependency_directory_empty "$staging" &&
            ! find "$staging" -type f \( -name .cup-deps-building -o -name .cup-dependencies \) \
                -print -quit | grep -q .; then
            echo "Error: dependency staging directory has no cup metadata: $staging" >&2
            return 1
        fi
        cup_path_remove_child_tree "$root" "$staging" \
            'dependency staging directory' || return 1
    fi
}

dependency_prepare_root() {
    local root="$1"

    dependency_validate_root_path "$root" || return 1
    if [ ! -e "$root" ] && [ ! -L "$root" ]; then
        cup_path_prepare_directory_chain "$root" "dependency root" || return 1
        dependency_write_root_marker "$root" || return 1
    elif ! cup_path_check_directory_chain "$root" 0 "dependency root"; then
        echo "Error: dependency root is not a real directory: $root" >&2
        return 1
    elif dependency_root_marker_valid "$root"; then
        :
    elif [ -e "$root/$CUP_DEPENDENCY_ROOT_MARKER" ] || [ -L "$root/$CUP_DEPENDENCY_ROOT_MARKER" ]; then
        echo "Error: dependency root marker is invalid: $root/$CUP_DEPENDENCY_ROOT_MARKER" >&2
        return 1
    else
        if dependency_directory_empty "$root"; then
            dependency_write_root_marker "$root" || return 1
        else
            dependency_root_entries_are_adoptable "$root" || return 1
            [ -d "$root/install" ] && dependency_prefix_owned "$root/install" || {
                echo "Error: non-empty dependency root has no verifiable cup prefix: $root" >&2
                return 1
            }
            dependency_write_root_marker "$root" || return 1
        fi
    fi
    dependency_recover_root "$root"
}

dependency_clean_root() {
    local root="$1"
    local status=0

    dependency_validate_root_path "$root" || return 1
    dependency_acquire_build_lock "$root" || return 1
    if [ ! -e "$root" ] && [ ! -L "$root" ]; then
        dependency_release_build_lock
        return 0
    fi
    if ! cup_path_check_directory_chain "$root" 0 "dependency root"; then
        status=1
    elif ! dependency_root_marker_valid "$root"; then
        echo "Error: refusing to remove an unowned dependency root: $root" >&2
        status=1
    elif ! cup_path_remove_directory_tree "$root" 'dependency root'; then
        status=1
    fi
    dependency_release_build_lock
    return "$status"
}

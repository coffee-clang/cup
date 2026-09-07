# Owns dependency-root validation, recovery and cleanup.
# Sourced by common.sh; not executable.

# Dependency prefix compatibility and transactional commit state.
# The root marker proves filesystem ownership; the prefix metadata records one
# compiled generation.
CUP_DEPS_PREFIX_READY=0
CUP_DEPS_FINAL_PREFIX=
CUP_DEPS_STAGE_ROOT=
CUP_DEPS_BUILD_PREFIX=
CUP_DEPS_USE_OPENSSL=1

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
    local file="$1"
    cmp -s "$file" <(cat)
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

dependency_root_marker_valid() {
    local root="$1"
    local marker="$root/$CUP_DEPENDENCY_ROOT_MARKER"
    [ -d "$root" ] && [ ! -L "$root" ] && [ -f "$marker" ] && [ ! -L "$marker" ] &&
        dependency_root_metadata | dependency_stream_matches_file "$marker"
}

dependency_write_root_marker() {
    local root="$1"
    local marker="$root/$CUP_DEPENDENCY_ROOT_MARKER"
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

dependency_recover_root() {
    local root="$1"
    local staging="$root/.install.staging"

    if [ -e "$staging" ] || [ -L "$staging" ]; then
        [ -d "$staging" ] && [ ! -L "$staging" ] || {
            echo "Error: dependency staging path is not a managed directory: $staging" >&2
            return 1
        }
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
            echo "Error: refusing to adopt a non-empty unmarked dependency root: $root" >&2
            return 1
        fi
    fi
    dependency_recover_root "$root"
}

dependency_clean_root() {
    local root="$1"

    dependency_validate_root_path "$root" || return 1
    if [ ! -e "$root" ] && [ ! -L "$root" ]; then
        return 0
    fi
    cup_path_check_directory_chain "$root" 0 "dependency root" || return 1
    if ! dependency_root_marker_valid "$root"; then
        echo "Error: refusing to remove an unowned dependency root: $root" >&2
        return 1
    fi
    cup_path_remove_directory_tree "$root" 'dependency root'
}

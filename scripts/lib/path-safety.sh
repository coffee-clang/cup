# Shared path guards and filesystem operations for repository-owned build trees.
# The repository assumes cooperative use; this layer prevents plausible path mistakes,
# symlink traversal through managed parents and accidental destructive cleanup.

CUP_PATH_WINDOWS=0
case "${OS:-}:$(uname -s 2>/dev/null || true)" in
    Windows_NT:*|*:MSYS*|*:MINGW*|*:CYGWIN*) CUP_PATH_WINDOWS=1 ;;
esac

cup_path_error() {
    printf 'Error: %s\n' "$*" >&2
    return 1
}

cup_path_validate_absolute_clean() {
    _cup_path_value=${1:-}
    _cup_path_label=${2:-path}

    case "$_cup_path_value" in
        '') cup_path_error "$_cup_path_label must not be empty"; return 1 ;;
        /*) ;;
        *) cup_path_error "$_cup_path_label must be absolute: $_cup_path_value"; return 1 ;;
    esac
    case "$_cup_path_value" in
        /) cup_path_error "$_cup_path_label must not be a filesystem root: $_cup_path_value"; return 1 ;;
        */) cup_path_error "$_cup_path_label must not end with '/': $_cup_path_value"; return 1 ;;
        *'\'*) cup_path_error "$_cup_path_label must use forward slashes: $_cup_path_value"; return 1 ;;
        *'
'*) cup_path_error "$_cup_path_label contains a newline"; return 1 ;;
        *"$(printf '\r')"*) cup_path_error "$_cup_path_label contains a carriage return"; return 1 ;;
    esac

    _cup_path_rest=${_cup_path_value#/}
    while [ -n "$_cup_path_rest" ]; do
        case "$_cup_path_rest" in
            */*) _cup_path_component=${_cup_path_rest%%/*}; _cup_path_rest=${_cup_path_rest#*/} ;;
            *) _cup_path_component=$_cup_path_rest; _cup_path_rest= ;;
        esac
        case "$_cup_path_component" in
            ''|.|..)
                cup_path_error "$_cup_path_label contains a non-canonical path component: $_cup_path_value"
                return 1
                ;;
        esac
    done

    if [ "$CUP_PATH_WINDOWS" -eq 1 ] && command -v cygpath >/dev/null 2>&1; then
        _cup_path_native=$(cygpath -w -- "$_cup_path_value" 2>/dev/null || true)
        if printf '%s\n' "$_cup_path_native" | grep -Eq '^[A-Za-z]:[\\/]?$'; then
            cup_path_error "$_cup_path_label must not be a filesystem root: $_cup_path_value"
            return 1
        fi
    fi
}

cup_path_check_directory_chain() {
    _cup_path_value=$1
    _cup_path_allow_missing=${2:-0}
    _cup_path_label=${3:-directory path}

    cup_path_validate_absolute_clean "$_cup_path_value" "$_cup_path_label" || return 1
    _cup_path_cursor=
    _cup_path_rest=${_cup_path_value#/}
    _cup_path_missing=0
    while [ -n "$_cup_path_rest" ]; do
        case "$_cup_path_rest" in
            */*) _cup_path_component=${_cup_path_rest%%/*}; _cup_path_rest=${_cup_path_rest#*/} ;;
            *) _cup_path_component=$_cup_path_rest; _cup_path_rest= ;;
        esac
        _cup_path_cursor=$_cup_path_cursor/$_cup_path_component
        if [ "$_cup_path_missing" -eq 1 ]; then
            continue
        fi
        if [ -L "$_cup_path_cursor" ]; then
            cup_path_error "unsafe symlink in $_cup_path_label: $_cup_path_cursor"
            return 1
        fi
        if [ -e "$_cup_path_cursor" ]; then
            [ -d "$_cup_path_cursor" ] || {
                cup_path_error "non-directory component in $_cup_path_label: $_cup_path_cursor"
                return 1
            }
        elif [ "$_cup_path_allow_missing" -eq 1 ]; then
            _cup_path_missing=1
        else
            cup_path_error "$_cup_path_label must be an existing directory: $_cup_path_value"
            return 1
        fi
    done
}

cup_path_prepare_directory_chain() {
    _cup_path_value=$1
    _cup_path_label=${2:-directory path}

    cup_path_validate_absolute_clean "$_cup_path_value" "$_cup_path_label" || return 1
    _cup_path_cursor=
    _cup_path_rest=${_cup_path_value#/}
    while [ -n "$_cup_path_rest" ]; do
        case "$_cup_path_rest" in
            */*) _cup_path_component=${_cup_path_rest%%/*}; _cup_path_rest=${_cup_path_rest#*/} ;;
            *) _cup_path_component=$_cup_path_rest; _cup_path_rest= ;;
        esac
        _cup_path_cursor=$_cup_path_cursor/$_cup_path_component
        if [ -L "$_cup_path_cursor" ]; then
            cup_path_error "unsafe symlink in $_cup_path_label: $_cup_path_cursor"
            return 1
        fi
        if [ -e "$_cup_path_cursor" ]; then
            [ -d "$_cup_path_cursor" ] || {
                cup_path_error "non-directory component in $_cup_path_label: $_cup_path_cursor"
                return 1
            }
        else
            if ! mkdir -- "$_cup_path_cursor" 2>/dev/null; then
                # A parallel repository job may have created the same directory
                # after the existence check. Accept only the resulting real directory.
                if [ -L "$_cup_path_cursor" ] || [ ! -d "$_cup_path_cursor" ]; then
                    cup_path_error "could not prepare $_cup_path_label: $_cup_path_cursor"
                    return 1
                fi
            fi
        fi
    done
}

cup_path_resolve_host_temporary_directory() {
    _cup_temp_label=${1:-host temporary directory}
    _cup_temp_value=${TMPDIR:-/tmp}

    if [ "$CUP_PATH_WINDOWS" -eq 1 ]; then
        case "$_cup_temp_value" in
            [A-Za-z]:[\\/]*)
                command -v cygpath >/dev/null 2>&1 || {
                    cup_path_error 'cygpath is required to normalize a native Windows temporary directory'
                    return 1
                }
                _cup_temp_value=$(cygpath -u -- "$_cup_temp_value") || {
                    cup_path_error "could not normalize $_cup_temp_label: ${TMPDIR:-/tmp}"
                    return 1
                }
                ;;
        esac
    fi
    while [ "$_cup_temp_value" != / ]; do
        case "$_cup_temp_value" in */) _cup_temp_value=${_cup_temp_value%/} ;; *) break ;; esac
    done
    case "$_cup_temp_value" in /*) ;; *) _cup_temp_value=$(pwd -P)/$_cup_temp_value ;; esac
    cup_path_validate_absolute_clean "$_cup_temp_value" "$_cup_temp_label" || return 1
    _cup_temp_value=$(CDPATH= cd -- "$_cup_temp_value" 2>/dev/null && pwd -P) || {
        cup_path_error "$_cup_temp_label must be an existing directory: $_cup_temp_value"
        return 1
    }
    cup_path_check_directory_chain "$_cup_temp_value" 0 "$_cup_temp_label" || return 1
    printf '%s\n' "$_cup_temp_value"
}

cup_path_require_within() {
    _cup_path_parent=$1
    _cup_path_child=$2
    _cup_path_label=${3:-path}
    cup_path_validate_absolute_clean "$_cup_path_parent" 'owned root' || return 1
    cup_path_validate_absolute_clean "$_cup_path_child" "$_cup_path_label" || return 1
    case "$_cup_path_child" in
        "$_cup_path_parent"/*) ;;
        *) cup_path_error "$_cup_path_label must stay inside $_cup_path_parent: $_cup_path_child"; return 1 ;;
    esac
}

cup_path_create_unique_directory() {
    _cup_unique_template=$1
    _cup_unique_label=${2:-temporary directory}
    _cup_unique_mode=${3:-0700}
    cup_path_validate_absolute_clean "$_cup_unique_template" "$_cup_unique_label template" || return 1
    case "$_cup_unique_template" in *XXXXXX) ;; *) cup_path_error "$_cup_unique_label template must end in XXXXXX: $_cup_unique_template"; return 1 ;; esac
    _cup_unique_parent=$(dirname -- "$_cup_unique_template") || return 1
    cup_path_check_directory_chain "$_cup_unique_parent" 0 "$_cup_unique_label parent" || return 1
    _cup_unique_created=$(mktemp -d "$_cup_unique_template") || {
        cup_path_error "could not create $_cup_unique_label from $_cup_unique_template"
        return 1
    }
    chmod "$_cup_unique_mode" "$_cup_unique_created" || { rm -rf -- "$_cup_unique_created"; return 1; }
    cup_path_require_within "$_cup_unique_parent" "$_cup_unique_created" "$_cup_unique_label" || return 1
    printf '%s\n' "$_cup_unique_created"
}

cup_path_prepare_child_directory() {
    _cup_child_root=$1
    _cup_child_directory=$2
    _cup_child_label=${3:-output directory}
    cup_path_check_directory_chain "$_cup_child_root" 0 'owned root' || return 1
    [ "$_cup_child_directory" = "$_cup_child_root" ] ||
        cup_path_require_within "$_cup_child_root" "$_cup_child_directory" "$_cup_child_label" || return 1
    cup_path_prepare_directory_chain "$_cup_child_directory" "$_cup_child_label"
}

cup_path_require_regular_file() {
    _cup_path_file=$1
    _cup_path_label=${2:-file}
    cup_path_validate_absolute_clean "$_cup_path_file" "$_cup_path_label" || return 1
    [ -f "$_cup_path_file" ] && [ ! -L "$_cup_path_file" ] || {
        cup_path_error "$_cup_path_label is not a safe regular file: $_cup_path_file"
        return 1
    }
}

cup_path_prepare_child_file() {
    _cup_child_file_root=$1
    _cup_child_file=$2
    _cup_child_file_label=${3:-output file}
    cup_path_require_within "$_cup_child_file_root" "$_cup_child_file" "$_cup_child_file_label" || return 1
    _cup_child_file_parent=$(dirname -- "$_cup_child_file") || return 1
    cup_path_prepare_child_directory "$_cup_child_file_root" "$_cup_child_file_parent" "$_cup_child_file_label parent" || return 1
    if [ -e "$_cup_child_file" ] || [ -L "$_cup_child_file" ]; then
        cup_path_require_regular_file "$_cup_child_file" "$_cup_child_file_label" || return 1
    fi
}

cup_path_prepare_file_target() {
    _cup_file_target=$1
    _cup_file_target_label=${2:-output file}
    cup_path_validate_absolute_clean "$_cup_file_target" "$_cup_file_target_label" || return 1
    _cup_file_target_parent=$(dirname -- "$_cup_file_target") || return 1
    cup_path_prepare_directory_chain "$_cup_file_target_parent" "$_cup_file_target_label parent" || return 1
    if [ -e "$_cup_file_target" ] || [ -L "$_cup_file_target" ]; then
        cup_path_require_regular_file "$_cup_file_target" "$_cup_file_target_label" || return 1
    fi
}

cup_path_require_safe_tree() {
    _cup_path_tree=$1
    _cup_path_label=${2:-directory tree}
    cup_path_check_directory_chain "$_cup_path_tree" 0 "$_cup_path_label" || return 1
    if find "$_cup_path_tree" \! -type d \! -type f -print -quit | grep -q .; then
        cup_path_error "$_cup_path_label contains a link or special entry: $_cup_path_tree"
        return 1
    fi
}

cup_path_write_file() (
    _cup_path_file=$1
    _cup_path_mode=${2:-0644}
    _cup_path_policy=${3:-replace}
    case "$_cup_path_policy" in replace|if-different|no-replace) ;; *) cup_path_error "unsupported write policy: $_cup_path_policy"; exit 1 ;; esac
    cup_path_validate_absolute_clean "$_cup_path_file" 'output file' || exit 1
    _cup_path_parent=$(dirname -- "$_cup_path_file") || exit 1
    cup_path_check_directory_chain "$_cup_path_parent" 0 'output file parent' || exit 1
    if [ -e "$_cup_path_file" ] || [ -L "$_cup_path_file" ]; then
        cup_path_require_regular_file "$_cup_path_file" 'output file' || exit 1
        [ "$_cup_path_policy" != no-replace ] || { cup_path_error "output file already exists: $_cup_path_file"; exit 1; }
    fi
    _cup_path_temp=$(mktemp "$_cup_path_parent/.cup-write.XXXXXX") || exit 1
    cleanup_cup_path_write() { rm -f -- "$_cup_path_temp"; }
    trap cleanup_cup_path_write EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    cat > "$_cup_path_temp" || exit 1
    chmod "$_cup_path_mode" "$_cup_path_temp" || exit 1
    if [ "$_cup_path_policy" = if-different ] && [ -f "$_cup_path_file" ] && cmp -s "$_cup_path_temp" "$_cup_path_file"; then
        exit 0
    fi
    if [ "$_cup_path_policy" = no-replace ]; then
        ln "$_cup_path_temp" "$_cup_path_file" 2>/dev/null || { cup_path_error "output file already exists or could not be published: $_cup_path_file"; exit 1; }
        exit 0
    fi
    mv -f -- "$_cup_path_temp" "$_cup_path_file" || exit 1
    _cup_path_temp=
)

cup_path_copy_file() {
    _cup_path_source=$1
    _cup_path_destination=$2
    _cup_path_mode=${3:-0644}
    _cup_path_policy=${4:-replace}
    cup_path_require_regular_file "$_cup_path_source" 'source file' || return 1
    cat -- "$_cup_path_source" | cup_path_write_file "$_cup_path_destination" "$_cup_path_mode" "$_cup_path_policy"
}

cup_path_copy_tree() {
    _cup_path_source=$1
    _cup_path_destination=$2
    cup_path_require_safe_tree "$_cup_path_source" 'source tree' || return 1
    cup_path_check_directory_chain "$_cup_path_destination" 0 'destination tree' || return 1
    cp -R -- "$_cup_path_source"/. "$_cup_path_destination"/ || {
        cup_path_error "could not copy tree: $_cup_path_source -> $_cup_path_destination"
        return 1
    }
}

cup_path_build_marker() {
    printf '%s\n' 'format=1' 'product=coffee-clang/cup' 'kind=build-root' 'layout=1'
}

cup_path_build_root_safe() {
    _cup_path_root=$1
    cup_path_validate_absolute_clean "$_cup_path_root" 'build root' || return 1
    if [ -n "${HOME:-}" ] && [ "$_cup_path_root" = "${HOME%/}" ]; then
        cup_path_error "build root must not be the user home directory: $_cup_path_root"
        return 1
    fi
}

cup_path_require_build_root() (
    _cup_path_root=$1
    cup_path_build_root_safe "$_cup_path_root" || exit 1
    cup_path_check_directory_chain "$_cup_path_root" 0 'build root' || exit 1
    _cup_path_marker=$_cup_path_root/.cup-build-root
    if [ ! -f "$_cup_path_marker" ] || [ -L "$_cup_path_marker" ] ||
        ! cup_path_build_marker | cmp -s - "$_cup_path_marker"; then
        cup_path_error "invalid build root marker: $_cup_path_marker"
        exit 1
    fi
)

cup_path_prepare_build_root() (
    _cup_path_root=$1
    cup_path_build_root_safe "$_cup_path_root" || exit 1
    _cup_path_parent=$(dirname -- "$_cup_path_root") || exit 1
    cup_path_prepare_directory_chain "$_cup_path_parent" 'build root parent' || exit 1
    if [ -e "$_cup_path_root" ] || [ -L "$_cup_path_root" ]; then
        cup_path_require_build_root "$_cup_path_root" || exit 1
        exit 0
    fi
    mkdir -- "$_cup_path_root" || exit 1
    if ! cup_path_build_marker | cup_path_write_file "$_cup_path_root/.cup-build-root" 0644 no-replace; then
        rm -rf -- "$_cup_path_root"
        exit 1
    fi
)

cup_path_clean_build_root() (
    _cup_path_root=$1
    cup_path_require_build_root "$_cup_path_root" || exit 1
    rm -rf -- "$_cup_path_root" || { cup_path_error "could not remove build root: $_cup_path_root"; exit 1; }
)

cup_path_move_entry() {
    _cup_path_source=$1
    _cup_path_destination=$2
    cup_path_validate_absolute_clean "$_cup_path_source" 'move source' || return 1
    cup_path_validate_absolute_clean "$_cup_path_destination" 'move destination' || return 1
    [ ! -L "$_cup_path_source" ] && [ -e "$_cup_path_source" ] || { cup_path_error "invalid move source: $_cup_path_source"; return 1; }
    [ ! -e "$_cup_path_destination" ] && [ ! -L "$_cup_path_destination" ] || { cup_path_error "move destination already exists: $_cup_path_destination"; return 1; }
    _cup_path_parent=$(dirname -- "$_cup_path_destination") || return 1
    cup_path_check_directory_chain "$_cup_path_parent" 0 'move destination parent' || return 1
    mv -- "$_cup_path_source" "$_cup_path_destination" || { cup_path_error "could not move entry: $_cup_path_source"; return 1; }
}

cup_path_remove_file() {
    _cup_path_file=$1
    _cup_path_label=${2:-file}
    cup_path_validate_absolute_clean "$_cup_path_file" "$_cup_path_label" || return 1
    if [ -L "$_cup_path_file" ]; then
        cup_path_error "refusing to remove symlink $_cup_path_label: $_cup_path_file"
        return 1
    fi
    if [ -e "$_cup_path_file" ] && [ ! -f "$_cup_path_file" ]; then
        cup_path_error "refusing to remove non-file $_cup_path_label: $_cup_path_file"
        return 1
    fi
    rm -f -- "$_cup_path_file" || { cup_path_error "could not remove $_cup_path_label: $_cup_path_file"; return 1; }
}

cup_path_remove_directory_tree() {
    _cup_path_tree=$1
    _cup_path_label=${2:-directory tree}
    cup_path_validate_absolute_clean "$_cup_path_tree" "$_cup_path_label" || return 1
    if [ -n "${HOME:-}" ] && [ "$_cup_path_tree" = "${HOME%/}" ]; then
        cup_path_error "refusing to remove HOME as $_cup_path_label: $_cup_path_tree"
        return 1
    fi
    if [ -n "${PROJECT_ROOT:-}" ] && [ "$_cup_path_tree" = "${PROJECT_ROOT%/}" ]; then
        cup_path_error "refusing to remove the checkout as $_cup_path_label: $_cup_path_tree"
        return 1
    fi
    [ ! -L "$_cup_path_tree" ] || { cup_path_error "refusing to remove symlink $_cup_path_label: $_cup_path_tree"; return 1; }
    [ ! -e "$_cup_path_tree" ] || [ -d "$_cup_path_tree" ] || { cup_path_error "$_cup_path_label is not a directory: $_cup_path_tree"; return 1; }
    rm -rf -- "$_cup_path_tree" || { cup_path_error "could not remove $_cup_path_label: $_cup_path_tree"; return 1; }
}

cup_path_remove_child_tree() {
    _cup_remove_root=$1
    _cup_remove_tree=$2
    _cup_remove_label=${3:-owned directory tree}
    cup_path_check_directory_chain "$_cup_remove_root" 0 'owned root' || return 1
    cup_path_require_within "$_cup_remove_root" "$_cup_remove_tree" "$_cup_remove_label" || return 1
    cup_path_remove_directory_tree "$_cup_remove_tree" "$_cup_remove_label"
}

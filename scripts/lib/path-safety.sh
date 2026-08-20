# Provides shared fd-relative path operations for owned build and release trees.
# This library is sourced by Makefile recipes and repository scripts.

# Never trust launcher/helper paths inherited from the ambient environment.
# These in-process caches are populated only after launcher validation below.
CUP_PATH_OPS_RESOLVED_LAUNCHER=
CUP_PATH_OPS_RESOLVED_HELPER=

cup_path_error() {
    printf 'Error: %s\n' "$*" >&2
    return 1
}

cup_path_find_ops_launcher() {
    if [ -n "${CUP_PATH_OPS_LAUNCHER:-}" ]; then
        [ "${CUP_PATH_OPS_TESTING:-0}" = 1 ] || {
            cup_path_error 'CUP_PATH_OPS_LAUNCHER is reserved for repository tests'
            return 1
        }
        case "$CUP_PATH_OPS_LAUNCHER" in
            /*) ;;
            *)
                cup_path_error "filesystem-helper launcher override must be absolute: $CUP_PATH_OPS_LAUNCHER"
                return 1
                ;;
        esac
        [ -f "$CUP_PATH_OPS_LAUNCHER" ] && [ -x "$CUP_PATH_OPS_LAUNCHER" ] &&
            [ ! -L "$CUP_PATH_OPS_LAUNCHER" ] || {
            cup_path_error "invalid CUP_PATH_OPS_LAUNCHER: $CUP_PATH_OPS_LAUNCHER"
            return 1
        }
        printf '%s\n' "$CUP_PATH_OPS_LAUNCHER"
        return 0
    fi

    for _cup_path_candidate in \
        "${PROJECT_ROOT:-}/scripts/lib/path-ops.sh" \
        "${SCRIPT_DIR:-}/path-ops.sh" \
        "${SCRIPT_DIR:-}/../lib/path-ops.sh" \
        "$(pwd -P 2>/dev/null)/scripts/lib/path-ops.sh"; do
        case "$_cup_path_candidate" in
            /*)
                if [ -f "$_cup_path_candidate" ] && [ ! -L "$_cup_path_candidate" ] &&
                    [ -x "$_cup_path_candidate" ]; then
                    printf '%s\n' "$_cup_path_candidate"
                    return 0
                fi
                ;;
        esac
    done
    cup_path_error 'could not locate scripts/lib/path-ops.sh'
    return 1
}

cup_path_resolve_ops_helper() {
    if [ -n "${CUP_PATH_OPS_RESOLVED_HELPER:-}" ]; then
        return 0
    fi

    CUP_PATH_OPS_RESOLVED_LAUNCHER=$(cup_path_find_ops_launcher) || return 1
    CUP_PATH_OPS_RESOLVED_HELPER=$("$CUP_PATH_OPS_RESOLVED_LAUNCHER" --print-helper) || return 1
    case "$CUP_PATH_OPS_RESOLVED_HELPER" in
        /*) ;;
        *)
            cup_path_error \
                "filesystem helper is not absolute: $CUP_PATH_OPS_RESOLVED_HELPER"
            CUP_PATH_OPS_RESOLVED_HELPER=
            return 1
            ;;
    esac
    if [ ! -f "$CUP_PATH_OPS_RESOLVED_HELPER" ] || \
        [ ! -x "$CUP_PATH_OPS_RESOLVED_HELPER" ] || \
        [ -L "$CUP_PATH_OPS_RESOLVED_HELPER" ]; then
        cup_path_error \
            "filesystem helper is not a safe executable: $CUP_PATH_OPS_RESOLVED_HELPER"
        CUP_PATH_OPS_RESOLVED_HELPER=
        return 1
    fi
}

cup_path_ops() {
    cup_path_resolve_ops_helper || return 1
    case "${1:-}" in
        mkdir-unique|run-build)
            "$CUP_PATH_OPS_RESOLVED_LAUNCHER" "$@"
            ;;
        *)
            "$CUP_PATH_OPS_RESOLVED_HELPER" "$@"
            ;;
    esac
}

cup_path_resolve_host_temporary_directory() {
    _cup_temp_label=${1:-host temporary directory}
    _cup_temp_value=${TMPDIR:-/tmp}

    while [ "$_cup_temp_value" != / ]; do
        case "$_cup_temp_value" in
            */) _cup_temp_value=${_cup_temp_value%/} ;;
            *) break ;;
        esac
    done
    case "$_cup_temp_value" in
        /*) ;;
        *) _cup_temp_value=$(pwd -P)/$_cup_temp_value ;;
    esac
    cup_path_validate_absolute_clean "$_cup_temp_value" "$_cup_temp_label" || return 1
    if ! _cup_temp_value=$(CDPATH= cd -- "$_cup_temp_value" 2>/dev/null && pwd -P); then
        cup_path_error "$_cup_temp_label must be an existing directory: $_cup_temp_value"
        return 1
    fi
    cup_path_validate_absolute_clean "$_cup_temp_value" "$_cup_temp_label" || return 1
    cup_path_check_directory_chain "$_cup_temp_value" 0 "$_cup_temp_label" || return 1
    printf '%s\n' "$_cup_temp_value"
}

cup_path_validate_absolute_clean() (
    _cup_path_value=${1:-}
    _cup_path_label=${2:-path}
    _cup_path_cr=$(printf '\r')
    _cup_path_lf='
'

    [ -n "$_cup_path_value" ] || {
        cup_path_error "$_cup_path_label must not be empty"
        exit 1
    }
    case "$_cup_path_value" in
        *"$_cup_path_lf"*|*"$_cup_path_cr"*)
            cup_path_error "$_cup_path_label must not contain line breaks: $_cup_path_value"
            exit 1
            ;;
        *\\*)
            cup_path_error "$_cup_path_label must use forward slashes: $_cup_path_value"
            exit 1
            ;;
        /)
            cup_path_error "$_cup_path_label must not be a filesystem root: $_cup_path_value"
            exit 1
            ;;
        /*)
            _cup_path_rest=${_cup_path_value#/}
            ;;
        *)
            cup_path_error "$_cup_path_label must be absolute: $_cup_path_value"
            exit 1
            ;;
    esac
    case "$_cup_path_value" in
        */)
            cup_path_error "$_cup_path_label must not have a trailing slash: $_cup_path_value"
            exit 1
            ;;
        *'//'*)
            cup_path_error "$_cup_path_label must not contain empty path components: $_cup_path_value"
            exit 1
            ;;
    esac

    _cup_path_old_ifs=$IFS
    IFS=/
    set -f
    set -- $_cup_path_rest
    set +f
    IFS=$_cup_path_old_ifs
    [ "$#" -gt 0 ] || {
        cup_path_error "$_cup_path_label has no owned path component: $_cup_path_value"
        exit 1
    }
    for _cup_path_component in "$@"; do
        case "$_cup_path_component" in
            ''|.|..)
                cup_path_error "$_cup_path_label contains an unsafe path component: $_cup_path_value"
                exit 1
                ;;
        esac
    done
)

cup_path_validate_relative_clean() (
    _cup_path_value=${1:-}
    _cup_path_label=${2:-path}
    _cup_path_cr=$(printf '\r')
    _cup_path_lf='
'

    [ -n "$_cup_path_value" ] || {
        cup_path_error "$_cup_path_label must not be empty"
        exit 1
    }
    case "$_cup_path_value" in
        /*|*"$_cup_path_lf"*|*"$_cup_path_cr"*|*\\*|*/|*'//'*)
            cup_path_error "$_cup_path_label is not a clean relative path: $_cup_path_value"
            exit 1
            ;;
    esac
    _cup_path_old_ifs=$IFS
    IFS=/
    set -f
    set -- $_cup_path_value
    set +f
    IFS=$_cup_path_old_ifs
    for _cup_path_component in "$@"; do
        case "$_cup_path_component" in
            ''|.|..)
                cup_path_error "$_cup_path_label contains an unsafe path component: $_cup_path_value"
                exit 1
                ;;
        esac
    done
)

cup_path_check_directory_chain() {
    _cup_path_value=$1
    _cup_path_allow_missing=${2:-0}
    _cup_path_label=${3:-directory path}

    cup_path_validate_absolute_clean "$_cup_path_value" "$_cup_path_label" || return 1
    if [ "$_cup_path_allow_missing" -eq 1 ]; then
        cup_path_ops check-dir "$_cup_path_value" allow-missing || {
            cup_path_error "unsafe or invalid $_cup_path_label: $_cup_path_value"
            return 1
        }
    else
        cup_path_ops check-dir "$_cup_path_value" || {
            cup_path_error "unsafe or invalid $_cup_path_label: $_cup_path_value"
            return 1
        }
    fi
}

cup_path_create_directory_exclusive() {
    _cup_path_value=$1
    _cup_path_label=${2:-directory path}
    cup_path_validate_absolute_clean "$_cup_path_value" "$_cup_path_label" || return 1
    cup_path_ops mkdir-exclusive "$_cup_path_value" || {
        cup_path_error "could not create exclusive $_cup_path_label: $_cup_path_value"
        return 1
    }
}

cup_path_prepare_directory_chain() {
    _cup_path_value=$1
    _cup_path_label=${2:-directory path}
    cup_path_validate_absolute_clean "$_cup_path_value" "$_cup_path_label" || return 1
    cup_path_ops ensure-dir "$_cup_path_value" || {
        cup_path_error "could not prepare $_cup_path_label: $_cup_path_value"
        return 1
    }
}

cup_path_create_unique_directory() {
    _cup_unique_template=$1
    _cup_unique_label=${2:-temporary directory}
    _cup_unique_mode=${3:-0700}
    cup_path_validate_absolute_clean "$_cup_unique_template" "$_cup_unique_label template" || return 1
    case "$_cup_unique_template" in
        *XXXXXX) ;;
        *) cup_path_error "$_cup_unique_label template must end in XXXXXX: $_cup_unique_template"; return 1 ;;
    esac
    _cup_unique_parent=$(dirname -- "$_cup_unique_template") || return 1
    _cup_unique_created=$(cup_path_ops mkdir-unique "$_cup_unique_template" "$_cup_unique_mode") || {
        cup_path_error "could not create $_cup_unique_label from $_cup_unique_template"
        return 1
    }
    cup_path_validate_absolute_clean "$_cup_unique_created" "$_cup_unique_label" || return 1
    cup_path_require_within "$_cup_unique_parent" "$_cup_unique_created" "$_cup_unique_label" || return 1
    cup_path_check_directory_chain "$_cup_unique_created" 0 "$_cup_unique_label" || return 1
    printf '%s\n' "$_cup_unique_created"
}

cup_path_require_within() (
    _cup_path_parent=$1
    _cup_path_child=$2
    _cup_path_label=${3:-path}

    cup_path_validate_absolute_clean "$_cup_path_parent" 'owned root' || exit 1
    cup_path_validate_absolute_clean "$_cup_path_child" "$_cup_path_label" || exit 1
    case "$_cup_path_child" in
        "$_cup_path_parent"/*) ;;
        *)
            cup_path_error "$_cup_path_label must stay inside $_cup_path_parent: $_cup_path_child"
            exit 1
            ;;
    esac
)

cup_path_prepare_child_directory() {
    _cup_child_root=$1
    _cup_child_directory=$2
    _cup_child_label=${3:-output directory}

    cup_path_check_directory_chain "$_cup_child_root" 0 'owned root' || return 1
    if [ "$_cup_child_directory" != "$_cup_child_root" ]; then
        cup_path_require_within "$_cup_child_root" "$_cup_child_directory" "$_cup_child_label" || return 1
    fi
    cup_path_prepare_directory_chain "$_cup_child_directory" "$_cup_child_label"
}

cup_path_prepare_child_file() {
    _cup_child_file_root=$1
    _cup_child_file=$2
    _cup_child_file_label=${3:-output file}

    cup_path_validate_absolute_clean "$_cup_child_file" "$_cup_child_file_label" || return 1
    cup_path_require_within "$_cup_child_file_root" "$_cup_child_file" "$_cup_child_file_label" || return 1
    _cup_child_file_parent=$(dirname -- "$_cup_child_file") || return 1
    cup_path_prepare_child_directory \
        "$_cup_child_file_root" "$_cup_child_file_parent" \
        "$_cup_child_file_label parent" || return 1
    if [ -e "$_cup_child_file" ] || [ -L "$_cup_child_file" ]; then
        cup_path_ops check-file "$_cup_child_file" || {
            cup_path_error "$_cup_child_file_label is not a safe regular file: $_cup_child_file"
            return 1
        }
    fi
}

cup_path_prepare_file_target() {
    _cup_file_target=$1
    _cup_file_target_label=${2:-output file}

    cup_path_validate_absolute_clean "$_cup_file_target" "$_cup_file_target_label" || return 1
    _cup_file_target_parent=$(dirname -- "$_cup_file_target") || return 1
    cup_path_prepare_directory_chain "$_cup_file_target_parent" "$_cup_file_target_label parent" || return 1
    if [ -e "$_cup_file_target" ] || [ -L "$_cup_file_target" ]; then
        cup_path_ops check-file "$_cup_file_target" || {
            cup_path_error "$_cup_file_target_label is not a safe regular file: $_cup_file_target"
            return 1
        }
    fi
}

cup_path_require_regular_file() {
    _cup_path_file=$1
    _cup_path_label=${2:-file}
    cup_path_validate_absolute_clean "$_cup_path_file" "$_cup_path_label" || return 1
    cup_path_ops check-file "$_cup_path_file" || {
        cup_path_error "$_cup_path_label is not a safe regular file: $_cup_path_file"
        return 1
    }
}

cup_path_require_safe_tree() {
    _cup_path_tree=$1
    _cup_path_label=${2:-directory tree}
    cup_path_validate_absolute_clean "$_cup_path_tree" "$_cup_path_label" || return 1
    cup_path_ops check-tree "$_cup_path_tree" || {
        cup_path_error "$_cup_path_label contains a link or special entry: $_cup_path_tree"
        return 1
    }
}

cup_path_write_file() {
    _cup_path_file=$1
    _cup_path_mode=${2:-0644}
    _cup_path_policy=${3:-replace}
    cup_path_validate_absolute_clean "$_cup_path_file" 'output file' || return 1
    case "$_cup_path_policy" in
        replace) cup_path_ops write-stdin "$_cup_path_file" "$_cup_path_mode" ;;
        if-different) cup_path_ops write-stdin "$_cup_path_file" "$_cup_path_mode" if-different ;;
        no-replace) cup_path_ops write-stdin "$_cup_path_file" "$_cup_path_mode" no-replace ;;
        *) cup_path_error "unsupported write policy: $_cup_path_policy"; return 1 ;;
    esac
}

cup_path_copy_file() {
    _cup_path_source=$1
    _cup_path_destination=$2
    _cup_path_mode=${3:-0644}
    _cup_path_policy=${4:-replace}
    cup_path_validate_absolute_clean "$_cup_path_source" 'copy source' || return 1
    cup_path_validate_absolute_clean "$_cup_path_destination" 'copy destination' || return 1
    case "$_cup_path_policy" in
        replace)
            cup_path_ops copy-file \
                "$_cup_path_source" "$_cup_path_destination" "$_cup_path_mode"
            ;;
        if-different)
            cup_path_ops copy-file \
                "$_cup_path_source" "$_cup_path_destination" "$_cup_path_mode" if-different
            ;;
        no-replace)
            cup_path_ops copy-file \
                "$_cup_path_source" "$_cup_path_destination" "$_cup_path_mode" no-replace
            ;;
        *) cup_path_error "unsupported copy policy: $_cup_path_policy"; return 1 ;;
    esac
}

cup_path_copy_tree() {
    _cup_path_source=$1
    _cup_path_destination=$2
    cup_path_validate_absolute_clean "$_cup_path_source" 'tree copy source' || return 1
    cup_path_validate_absolute_clean "$_cup_path_destination" 'tree copy destination' || return 1
    cup_path_ops copy-tree "$_cup_path_source" "$_cup_path_destination"
}


cup_path_prepare_build_root() {
    _cup_path_root=$1
    cup_path_validate_absolute_clean "$_cup_path_root" 'build root' || return 1
    cup_path_ops prepare-build-root "$_cup_path_root"
}

cup_path_run_build() {
    _cup_path_root=$1
    shift
    [ "${1:-}" = -- ] || {
        cup_path_error 'locked build command requires -- before the command'
        return 1
    }
    shift
    [ "$#" -gt 0 ] || {
        cup_path_error 'locked build command is missing'
        return 1
    }
    cup_path_validate_absolute_clean "$_cup_path_root" 'build root' || return 1
    cup_path_ops run-build "$_cup_path_root" -- "$@"
}

cup_path_clean_build_root() {
    _cup_path_root=$1
    cup_path_validate_absolute_clean "$_cup_path_root" 'build root' || return 1
    cup_path_ops clean-build-root "$_cup_path_root"
}

cup_path_move_entry() {
    _cup_path_source=$1
    _cup_path_destination=$2
    cup_path_validate_absolute_clean "$_cup_path_source" 'move source' || return 1
    cup_path_validate_absolute_clean "$_cup_path_destination" 'move destination' || return 1
    cup_path_ops move "$_cup_path_source" "$_cup_path_destination"
}

cup_path_remove_empty_directory() {
    _cup_path_directory=$1
    _cup_path_label=${2:-directory}
    cup_path_validate_absolute_clean "$_cup_path_directory" "$_cup_path_label" || return 1
    cup_path_ops rmdir "$_cup_path_directory" || {
        cup_path_error "could not remove empty $_cup_path_label: $_cup_path_directory"
        return 1
    }
}

cup_path_remove_file() {
    _cup_path_file=$1
    _cup_path_label=${2:-file}
    cup_path_validate_absolute_clean "$_cup_path_file" "$_cup_path_label" || return 1
    cup_path_ops remove-file "$_cup_path_file" || {
        cup_path_error "could not remove $_cup_path_label: $_cup_path_file"
        return 1
    }
}

cup_path_require_build_root() (
    _cup_path_root=$1
    cup_path_validate_absolute_clean "$_cup_path_root" 'build root' || exit 1
    cup_path_ops check-build-root "$_cup_path_root"
)

cup_path_remove_directory_tree() {
    _cup_path_tree=$1
    _cup_path_label=${2:-directory tree}
    cup_path_validate_absolute_clean "$_cup_path_tree" "$_cup_path_label" || return 1
    cup_path_ops remove-tree "$_cup_path_tree" || {
        cup_path_error "could not remove $_cup_path_label: $_cup_path_tree"
        return 1
    }
}

cup_path_remove_child_tree() {
    _cup_remove_root=$1
    _cup_remove_tree=$2
    _cup_remove_label=${3:-owned directory tree}

    cup_path_check_directory_chain "$_cup_remove_root" 0 'owned root' || return 1
    cup_path_require_within "$_cup_remove_root" "$_cup_remove_tree" "$_cup_remove_label" || return 1
    cup_path_remove_directory_tree "$_cup_remove_tree" "$_cup_remove_label"
}

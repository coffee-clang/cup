# Coverage mechanics shared by build scripts, the runner and repository regressions.

# GCC runtime profile relocation strips leading path components from the hardwired
# data-file path before adding the destination prefix. Windows MinGW builds hardwire
# the MSYS drive mount as one leading level, while cygpath supplies the runtime prefix
# as D:/...; account for that level explicitly. A POSIX root slash is only a separator.
cup_coverage_gcov_strip_components() {
    [ "$#" -eq 1 ] && [ -n "$1" ] || return 2
    cup_gcov_path=$(printf '%s\n' "$1" | tr '\\' '/') || return 2
    cup_gcov_extra_level=0
    case "$cup_gcov_path" in
        [A-Za-z]:/*)
            cup_gcov_path=${cup_gcov_path#??}
            cup_gcov_extra_level=1
            ;;
        /*) cup_gcov_path=${cup_gcov_path#/} ;;
        *) return 2 ;;
    esac
    [ -n "$cup_gcov_path" ] || return 2
    printf '%s\n' "$cup_gcov_path" | awk -F/ -v extra="$cup_gcov_extra_level" '{
        count = extra
        for (field = 1; field <= NF; field++) {
            if (length($field) != 0) {
                count++
            }
        }
        print count
    }'
}

cup_coverage_verify_gcov_profile_owners() {
    cup_gcov_tests_root=$1

    cup_gcov_retired=$(find "$cup_gcov_tests_root" -maxdepth 1 \
        \( -name '.unit.*' -o -name '.helpers.*' \) -print -quit)
    [ -z "$cup_gcov_retired" ] || {
        printf 'GCC coverage recreated retired build staging: %s\n' \
            "$cup_gcov_retired" >&2
        return 1
    }

    for cup_gcov_owner in \
        "$cup_gcov_tests_root/unit" \
        "$cup_gcov_tests_root/helpers"; do
        [ -d "$cup_gcov_owner" ] && [ ! -L "$cup_gcov_owner" ] || {
            printf 'Coverage profile owner is missing or invalid: %s\n' \
                "$cup_gcov_owner" >&2
            return 1
        }

        cup_gcov_unexpected=$(find "$cup_gcov_owner" -mindepth 1 ! -type f \
            -print -quit)
        [ -z "$cup_gcov_unexpected" ] || {
            printf 'Unexpected non-file entry in GCC coverage owner: %s\n' \
                "$cup_gcov_unexpected" >&2
            return 1
        }

        for cup_gcov_profile in "$cup_gcov_owner"/*.gcda; do
            [ -f "$cup_gcov_profile" ] || continue
            [ ! -L "$cup_gcov_profile" ] || {
                printf 'GCC coverage profile must not be a symlink: %s\n' \
                    "$cup_gcov_profile" >&2
                return 1
            }
            cup_gcov_note=${cup_gcov_profile%.gcda}.gcno
            [ -f "$cup_gcov_note" ] && [ ! -L "$cup_gcov_note" ] || {
                printf 'Coverage profile %s has no note in the same final owner.\n' \
                    "$cup_gcov_profile" >&2
                return 1
            }
        done
    done
}

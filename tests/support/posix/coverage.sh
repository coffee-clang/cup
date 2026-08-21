# Coverage mechanics shared by build scripts, the runner and repository regressions.

# MinGW GCC combines its native getpwd() result with a relative output using a
# DOS separator before applying -fprofile-prefix-path. The trailing POSIX
# separator belongs to the relative output path and must be part of the prefix.
cup_coverage_mingw_profile_prefix() {
    [ "$#" -eq 2 ] && [ -n "$1" ] && [ -n "$2" ] || return 2
    printf '%s\\%s/\n' "$1" "$2"
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

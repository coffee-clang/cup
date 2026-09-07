
# Owns CUP's canonical MAJOR.MINOR.PATCH semantic-version domain.
# Sourced by version and release tooling.

cup_semver_valid() (
    value=${1:-}
    case "$value" in
        ''|*[!0-9.]*|.*|*..*|*.) return 1 ;;
    esac
    old_ifs=$IFS
    IFS=.
    set -- $value
    IFS=$old_ifs
    [ "$#" -eq 3 ] || return 1
    for part in "$@"; do
        case "$part" in
            ''|*[!0-9]*) return 1 ;;
            0) ;;
            0*) return 1 ;;
        esac
        [ "${#part}" -le 6 ] || return 1
        [ "$part" -le 999999 ] || return 1
    done
)

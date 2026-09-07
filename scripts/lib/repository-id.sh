
# Owns the owner/repository identifier grammar for repository-hosted tooling.

cup_repository_identifier_valid() (
    value=${1-}
    case "$value" in
        */*) ;;
        *) return 1 ;;
    esac

    owner=${value%%/*}
    name=${value#*/}
    [ "$name" = "${name#*/}" ] || return 1
    for component in "$owner" "$name"; do
        case "$component" in
            ''|.|..|*[!A-Za-z0-9_.-]*) return 1 ;;
        esac
    done
)


# Owns the full Git commit identifier grammar used by repository-hosted tooling.

cup_git_commit_valid() (
    value=${1-}
    [ "${#value}" -eq 40 ] || return 1
    case "$value" in
        ''|*[!0-9a-f]*) return 1 ;;
    esac
)

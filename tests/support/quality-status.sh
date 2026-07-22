# Purpose: Shared quality-runner status precedence. Sourced by coverage tests.

cup_quality_final_status() {
    for status in "$@"; do
        case "$status" in
            ''|*[!0-9]*)
                return 2
                ;;
        esac

        if [ "$status" -ne 0 ]; then
            printf '%s\n' "$status"
            return 0
        fi
    done

    printf '0\n'
}

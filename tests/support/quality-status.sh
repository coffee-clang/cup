# Purpose: Shared quality-runner status precedence. Sourced by coverage tests.
cup_quality_final_status() {
    for status in "$@"; do
        case "$status" in ''|*[!0-9]*) return 2 ;; esac
        [ "$status" -eq 0 ] || { printf '%s\n' "$status"; return 0; }
    done
    printf '0\n'
}

# Purpose: Shared POSIX assertions for the detached uninstall lifecycle.
# This file is sourced, not executed.

cup_test_uninstall_residue() {
    parent=$1
    for candidate in "$parent"/.cup-uninstall.*; do
        if [ -e "$candidate" ] || [ -L "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
}

cup_test_wait_for_uninstall() {
    cup_root=$1
    parent=$2
    attempts=${3:-200}
    delay=${4:-0.1}
    attempt=0

    while [ "$attempt" -lt "$attempts" ]; do
        residue=$(cup_test_uninstall_residue "$parent")
        if [ ! -e "$cup_root" ] && [ ! -L "$cup_root" ] && [ -z "$residue" ]; then
            return 0
        fi
        sleep "$delay"
        attempt=$((attempt + 1))
    done
    return 1
}

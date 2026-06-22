#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

test_begin uninstall
uninstall_home=$TMP_ROOT/home
uninstall_root=$uninstall_home/.cup
helper=$TMP_ROOT/uninstall-helper.sh

mkdir -p "$uninstall_root/components"
printf 'fixture\n' > "$uninstall_root/components/file"
cp "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" "$helper"
chmod +x "$helper"

sh -c 'exit 0' &
dead_pid=$!
wait "$dead_pid"

HOME="$uninstall_home" "$helper" "$uninstall_root" "$helper" "$dead_pid"
assert_missing "$uninstall_root"
assert_missing "$helper"
if find "$uninstall_home" -name '.cup-uninstall.*' -print | \
    grep . >/dev/null 2>&1; then
    fail 'uninstall helper left its staging directory behind'
fi

printf '%s\n' 'Detached uninstall cleanup tests passed.'

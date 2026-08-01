#!/bin/sh

# Purpose: Detached POSIX helper that removes the canonical CUP root after the parent exits.
# Inputs: selected root, copied helper path, parent process id and inherited signal descriptor.
set -u

CUP_ROOT="${1:-}"
SELF_PATH="${2:-}"
PARENT_PID="${3:-}"
PARENT_SIGNAL_FD="${4:-}"
JOURNAL_ROOT="$CUP_ROOT"
TEMPORARY_NAME=
TOKEN=

read_exact_three_lines() {
    file=$1
    FILE_LINE_1=
    FILE_LINE_2=
    FILE_LINE_3=
    FILE_EXTRA_LINE=
    {
        IFS= read -r FILE_LINE_1 || return 1
        IFS= read -r FILE_LINE_2 || return 1
        IFS= read -r FILE_LINE_3 || return 1
        if IFS= read -r FILE_EXTRA_LINE; then
            return 1
        fi
    } < "$file"
}

read_exact_seven_lines() {
    file=$1
    FILE_LINE_1=
    FILE_LINE_2=
    FILE_LINE_3=
    FILE_LINE_4=
    FILE_LINE_5=
    FILE_LINE_6=
    FILE_LINE_7=
    FILE_EXTRA_LINE=
    {
        IFS= read -r FILE_LINE_1 || return 1
        IFS= read -r FILE_LINE_2 || return 1
        IFS= read -r FILE_LINE_3 || return 1
        IFS= read -r FILE_LINE_4 || return 1
        IFS= read -r FILE_LINE_5 || return 1
        IFS= read -r FILE_LINE_6 || return 1
        IFS= read -r FILE_LINE_7 || return 1
        if IFS= read -r FILE_EXTRA_LINE; then
            return 1
        fi
    } < "$file"
}

write_journal() {
    root=$1
    phase=$2
    stage=$3
    error=$4
    temporary="$root/.uninstall-transaction-$PARENT_PID"

    [ -d "$root" ] && [ ! -L "$root" ] || return 1
    if ! printf 'format=1\noperation=uninstall\nphase=%s\ntemporary_name=%s\ntoken=%s\nstage=%s\nerror=%s\n' \
        "$phase" "$TEMPORARY_NAME" "$TOKEN" "$stage" "$error" > "$temporary"; then
        rm -f "$temporary" 2>/dev/null || true
        return 1
    fi
    chmod 0600 "$temporary" 2>/dev/null || {
        rm -f "$temporary" 2>/dev/null || true
        return 1
    }
    mv -f "$temporary" "$root/transaction.txt" 2>/dev/null || {
        rm -f "$temporary" 2>/dev/null || true
        return 1
    }
}

fail() {
    stage=${1:-handoff}
    shift || true
    if [ -n "$TEMPORARY_NAME" ] && [ -n "$TOKEN" ]; then
        write_journal "$JOURNAL_ROOT" failed "$stage" 1 || true
    fi
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || {
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    }
}

for required_command in chmod mv rm rmdir; do
    need_command "$required_command"
done

[ -n "${HOME:-}" ] || fail handoff "HOME is not set"
case "$HOME" in
    /) fail handoff "HOME must not be the filesystem root" ;;
    /*) ;;
    *) fail handoff "HOME must contain an absolute path" ;;
esac
case "$CUP_ROOT" in
    "$HOME/.cup"|"$HOME/.coffee-cup") ;;
    *) fail handoff "refusing to remove an unsupported cup root" ;;
esac
case "$PARENT_PID" in
    ''|*[!0-9]*) fail handoff "invalid parent process id" ;;
esac
[ "$PARENT_PID" -gt 0 ] || fail handoff "invalid parent process id"
[ "$PARENT_SIGNAL_FD" = 3 ] || fail handoff "invalid parent lifetime signal"
[ -n "$SELF_PATH" ] && [ "$SELF_PATH" = "$0" ] ||
    fail handoff "self path does not match the running uninstall helper"
[ -f "$SELF_PATH" ] && [ ! -L "$SELF_PATH" ] ||
    fail handoff "the running uninstall helper is not a regular file"
[ -d "$CUP_ROOT" ] && [ ! -L "$CUP_ROOT" ] ||
    fail handoff "canonical cup root is not a real directory"

ROOT_MARKER="$CUP_ROOT/root.txt"
[ -f "$ROOT_MARKER" ] && [ ! -L "$ROOT_MARKER" ] ||
    fail handoff "cup root marker is missing or invalid"
read_exact_three_lines "$ROOT_MARKER" &&
    [ "$FILE_LINE_1" = "format=1" ] &&
    [ "$FILE_LINE_2" = "product=coffee-clang/cup" ] &&
    [ "$FILE_LINE_3" = "layout=1" ] ||
    fail handoff "cup root marker is missing or invalid"

JOURNAL="$CUP_ROOT/transaction.txt"
[ -f "$JOURNAL" ] && [ ! -L "$JOURNAL" ] ||
    fail handoff "uninstall transaction is missing or invalid"
read_exact_seven_lines "$JOURNAL" ||
    fail handoff "uninstall transaction is missing or invalid"
[ "$FILE_LINE_1" = "format=1" ] &&
    [ "$FILE_LINE_2" = "operation=uninstall" ] &&
    [ "$FILE_LINE_3" = "phase=scheduled" ] &&
    [ "${FILE_LINE_4#temporary_name=}" != "$FILE_LINE_4" ] &&
    [ "${FILE_LINE_5#token=}" != "$FILE_LINE_5" ] &&
    [ "$FILE_LINE_6" = "stage=handoff" ] &&
    [ "$FILE_LINE_7" = "error=0" ] ||
    fail handoff "uninstall transaction is missing or invalid"
TEMPORARY_NAME=${FILE_LINE_4#temporary_name=}
TOKEN=${FILE_LINE_5#token=}
case "$TOKEN" in
    ''|*[!A-Za-z0-9_-]*) fail handoff "uninstall token is invalid" ;;
esac
[ "$TEMPORARY_NAME" = ".cup-uninstall.$TOKEN" ] ||
    fail handoff "uninstall transaction identity is invalid"
DETACHED_ROOT="$HOME/$TEMPORARY_NAME"
[ ! -e "$DETACHED_ROOT" ] && [ ! -L "$DETACHED_ROOT" ] ||
    fail handoff "uninstall destination already exists"

# Persist the exact stage before acknowledging the handoff. Descriptor 3, not the PID, is the
# parent-lifetime signal.
write_journal "$CUP_ROOT" scheduled parent-wait 0 ||
    fail handoff "could not persist uninstall handoff"
printf 'R' >&3 || fail parent-wait "could not acknowledge uninstall handoff"
while IFS= read -r parent_message <&3; do
    :
done
exec 3<&-

write_journal "$CUP_ROOT" detaching detach 0 ||
    fail parent-wait "could not persist uninstall detach stage"
if ! mv "$CUP_ROOT" "$DETACHED_ROOT"; then
    fail detach "could not detach $CUP_ROOT"
fi
JOURNAL_ROOT="$DETACHED_ROOT"

# Keep the three ownership proofs required by the installer until every unrelated payload entry
# is gone. A cleanup failure must leave a residue that can still be recognized and retried.
write_journal "$DETACHED_ROOT" failed cleanup 1 ||
    fail cleanup "could not persist uninstall cleanup state"
for detached_entry in \
    "$DETACHED_ROOT"/* "$DETACHED_ROOT"/.[!.]* "$DETACHED_ROOT"/..?*; do
    [ -e "$detached_entry" ] || [ -L "$detached_entry" ] || continue
    case "${detached_entry##*/}" in
        transaction.txt|root.txt|bin) continue ;;
    esac
    rm -rf "$detached_entry" || fail cleanup "could not remove detached cup installation"
done
for binary_entry in \
    "$DETACHED_ROOT/bin"/* "$DETACHED_ROOT/bin"/.[!.]* "$DETACHED_ROOT/bin"/..?*; do
    [ -e "$binary_entry" ] || [ -L "$binary_entry" ] || continue
    [ "${binary_entry##*/}" = cup ] && continue
    rm -rf "$binary_entry" || fail cleanup "could not remove detached cup binaries"
done

# Only the final, already-minimal residue loses its ownership evidence.
rm -f "$DETACHED_ROOT/bin/cup" || fail cleanup "could not remove detached cup executable"
rmdir "$DETACHED_ROOT/bin" || fail cleanup "could not remove detached cup binary directory"
rm -f "$DETACHED_ROOT/root.txt" || fail cleanup "could not remove detached cup root marker"
rm -f "$DETACHED_ROOT/transaction.txt" ||
    fail cleanup "could not remove detached cup uninstall journal"
rmdir "$DETACHED_ROOT" || fail cleanup "could not remove detached cup installation"

rm -f "$SELF_PATH" || exit 1
exit 0

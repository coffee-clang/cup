#!/usr/bin/env bash
# Purpose: Exercises dependency path neutralization and compiled-archive guards.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cup-dependency-contract.XXXXXX")
trap 'rm -rf "$TMP_ROOT"' EXIT HUP INT TERM
# shellcheck source=../../scripts/dependencies/common.sh
source "$ROOT/scripts/dependencies/common.sh"

compiler=${CC:-cc}
command -v "$compiler" >/dev/null 2>&1 || {
    echo "dependency contract requires a C compiler: $compiler" >&2
    exit 1
}

maps=$(dependency_reproducible_cflags "$compiler" "$TMP_ROOT/source-root")
case " $maps " in *' -O2 '*) ;; *) echo 'dependency flags lost release optimization' >&2; exit 1;; esac
for option in file debug macro; do
    case "$maps" in *"-f${option}-prefix-map=$TMP_ROOT/source-root="*) ;;
        *) echo "dependency flags are missing ${option}-prefix-map" >&2; exit 1;;
    esac
done

prefix="$TMP_ROOT/prefix"
mkdir -p "$prefix/lib"
printf '%s\n' '/__cup_runtime__/openssl' > "$prefix/lib/libneutral.a"
dependency_compiled_paths_valid "$prefix" "$TMP_ROOT/forbidden"

printf '%s\n' "$TMP_ROOT/forbidden/source.c" > "$prefix/lib/libleak.a"
if dependency_compiled_paths_valid "$prefix" "$TMP_ROOT/forbidden" \
        >"$TMP_ROOT/leak.out" 2>&1; then
    echo 'compiled dependency guard accepted a POSIX path leak' >&2
    exit 1
fi
grep -Fq 'contains forbidden path' "$TMP_ROOT/leak.out"
rm -f "$prefix/lib/libleak.a"

mkdir -p "$TMP_ROOT/bin"
cat > "$TMP_ROOT/bin/cygpath" <<'EOF_CYGPATH'
#!/bin/sh
case "$1" in
  -m) printf '%s\n' 'C:/runner/deps' ;;
  -w) printf '%s\n' 'C:\\runner\\deps' ;;
  *) exit 2 ;;
esac
EOF_CYGPATH
chmod +x "$TMP_ROOT/bin/cygpath"
printf '%s\n' 'C:/runner/deps/build/source.c' > "$prefix/lib/libwindows-leak.a"
if PATH="$TMP_ROOT/bin:$PATH" dependency_compiled_paths_valid \
        "$prefix" "$TMP_ROOT/forbidden" >"$TMP_ROOT/windows.out" 2>&1; then
    echo 'compiled dependency guard accepted an MSYS2 drive-letter path leak' >&2
    exit 1
fi
grep -Fq 'C:/runner/deps' "$TMP_ROOT/windows.out"

if dependency_require_whitespace_free_path "test path" "$TMP_ROOT/with space" \
        >"$TMP_ROOT/space.out" 2>&1; then
    echo 'dependency path validation accepted whitespace' >&2
    exit 1
fi
grep -Fq 'must not contain whitespace' "$TMP_ROOT/space.out"

first_id=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-id)
second_id=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-id)
[ "$first_id" = "$second_id" ] || {
    echo 'dependency identity is not deterministic' >&2
    exit 1
}
case "$first_id" in
    *[!0-9a-f]*) echo 'dependency identity is not hexadecimal' >&2; exit 1 ;;
esac
[ "${#first_id}" -eq 64 ] || {
    echo 'dependency identity must contain 64 hexadecimal characters' >&2
    exit 1
}

grep -Fq -- "--proto '=https' --proto-redir '=https'" \
    "$ROOT/scripts/dependencies/common.sh" || {
    echo 'dependency downloads do not restrict initial and redirect protocols to HTTPS' >&2
    exit 1
}
for package in zlib xz openssl curl libarchive argtable3 uthash unity libevent; do
    case "$(source_url_for_package "$package")" in
        https://*) ;;
        *) echo "dependency source is not HTTPS: $package" >&2; exit 1 ;;
    esac
done

printf '%s\n' 'Dependency path-neutralization, identity and transport tests passed.'

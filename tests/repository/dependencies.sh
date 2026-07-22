#!/usr/bin/env bash
# Purpose: Exercises dependency path neutralization and compiled-archive guards.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cup-dependency-contract.XXXXXX")
trap 'rm -rf "$TMP_ROOT"' EXIT HUP INT TERM
# shellcheck source=../../scripts/dependencies/common.sh
source "$ROOT/scripts/dependencies/common.sh"

compiler=${CC:-cc}
archiver=${AR:-ar}
command -v "$compiler" >/dev/null 2>&1 || {
    echo "dependency contract requires a C compiler: $compiler" >&2
    exit 1
}
command -v "$archiver" >/dev/null 2>&1 || {
    echo "dependency contract requires a static archiver: $archiver" >&2
    exit 1
}

maps=$(dependency_reproducible_cflags "$compiler" "$TMP_ROOT/source-root")
case " $maps " in
    *' -O2 '*)
        ;;
    *)
        echo 'dependency flags lost release optimization' >&2
        exit 1
        ;;
esac
for option in file debug macro; do
    case "$maps" in
        *"-f${option}-prefix-map=$TMP_ROOT/source-root="*)
            ;;
        *)
            echo "dependency flags are missing ${option}-prefix-map" >&2
            exit 1
            ;;
    esac
done

buildinfo_flags=$(dependency_buildinfo_safe_cflags "$maps -fPIC")
case " $buildinfo_flags " in
    *' -O2 '*) ;;
    *)
        echo 'build-info flags lost release optimization' >&2
        exit 1
        ;;
esac
case " $buildinfo_flags " in
    *' -fPIC '*) ;;
    *)
        echo 'build-info flags lost path-independent options' >&2
        exit 1
        ;;
esac
case "$buildinfo_flags" in
    *prefix-map=*|*"$TMP_ROOT/source-root"*)
        echo 'build-info flags retained a path-bearing prefix map' >&2
        exit 1
        ;;
esac

buildinfo_prefix="$TMP_ROOT/buildinfo-prefix"
mkdir -p "$buildinfo_prefix/lib"
cat >"$TMP_ROOT/openssl-buildinfo.c" <<EOF_OPENSSL_BUILDINFO
const char cup_openssl_buildinfo[] = "compiler: $buildinfo_flags";
EOF_OPENSSL_BUILDINFO
"$compiler" -c "$TMP_ROOT/openssl-buildinfo.c" -o "$TMP_ROOT/openssl-buildinfo.o"
"$archiver" rcs "$buildinfo_prefix/lib/libcrypto.a" "$TMP_ROOT/openssl-buildinfo.o"
dependency_compiled_paths_valid "$buildinfo_prefix" "$TMP_ROOT/source-root"

cat >"$TMP_ROOT/openssl-leaking-buildinfo.c" <<EOF_OPENSSL_LEAK
const char cup_openssl_buildinfo[] = "compiler: $maps";
EOF_OPENSSL_LEAK
"$compiler" -c "$TMP_ROOT/openssl-leaking-buildinfo.c" \
    -o "$TMP_ROOT/openssl-leaking-buildinfo.o"
"$archiver" rcs "$buildinfo_prefix/lib/libcrypto-leak.a" \
    "$TMP_ROOT/openssl-leaking-buildinfo.o"
if dependency_compiled_paths_valid "$buildinfo_prefix" "$TMP_ROOT/source-root" \
        >"$TMP_ROOT/openssl-buildinfo.out" 2>&1; then
    echo 'compiled dependency guard accepted OpenSSL-style CFLAGS leakage' >&2
    exit 1
fi
grep -Fq 'contains forbidden path' "$TMP_ROOT/openssl-buildinfo.out"
rm -f "$buildinfo_prefix/lib/libcrypto-leak.a"

if ! grep -Fq 'openssl_cflags=$(dependency_buildinfo_safe_cflags "$CUP_DEPENDENCY_CFLAGS")' \
        "$ROOT/scripts/dependencies/build-posix.sh" ||
    ! grep -Fq 'CFLAGS="$openssl_cflags"' \
        "$ROOT/scripts/dependencies/build-posix.sh"; then
    echo 'OpenSSL build does not apply build-info-safe compiler flags' >&2
    exit 1
fi

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
  -m)
      printf '%s\n' 'C:/runner/deps'
      ;;
  -w)
      printf '%s\n' 'C:\\runner\\deps'
      ;;
  *)
      exit 2
      ;;
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

metadata_final="$TMP_ROOT/metadata-root/install"
metadata_stage="$TMP_ROOT/.install.staging.fixture"
metadata_prefix="$metadata_stage$metadata_final"
mkdir -p "$metadata_prefix/bin" "$metadata_prefix/lib/pkgconfig"
cat >"$metadata_prefix/bin/curl-config" <<EOF_METADATA_CONFIG
#!/bin/sh
CURL_BOOTSTRAP_CFLAGS='-O2 \
-ffile-prefix-map=$metadata_stage=/usr/src/cup-dependencies \
-fdebug-prefix-map=$metadata_stage=/usr/src/cup-dependencies \
-fmacro-prefix-map=$metadata_stage=/usr/src/cup-dependencies'
printf '%s\n' '-L$metadata_prefix/lib -lcurl'
EOF_METADATA_CONFIG
chmod +x "$metadata_prefix/bin/curl-config"
cat >"$metadata_prefix/lib/pkgconfig/libfixture.pc" <<EOF_METADATA_PC
prefix=$metadata_prefix
libdir=\${prefix}/lib
Name: fixture
Description: dependency normalization fixture
Version: 1
Cflags: -ffile-prefix-map=$metadata_stage=/usr/src/cup-dependencies -I\${prefix}/include
Libs: -L\${libdir} -lfixture
EOF_METADATA_PC
CUP_DEPS_STAGE_ROOT="$metadata_stage"
normalize_dependency_metadata "$metadata_prefix" "$metadata_prefix" "$metadata_final"
grep -Fq -- "-L$metadata_final/lib" "$metadata_prefix/bin/curl-config"
if grep -R -F -q -- "$metadata_stage" "$metadata_prefix/bin" "$metadata_prefix/lib/pkgconfig"; then
    echo 'dependency metadata normalization retained the staging root' >&2
    exit 1
fi
if grep -R -F -q -- 'prefix-map=' "$metadata_prefix/bin" "$metadata_prefix/lib/pkgconfig"; then
    echo 'dependency metadata normalization retained build-only prefix-map flags' >&2
    exit 1
fi
printf 'raw_stage=%s\n' "$metadata_stage" >"$metadata_prefix/lib/pkgconfig/leak.cmake"
if normalize_dependency_metadata "$metadata_prefix" "$metadata_final" "$metadata_final" \
        >"$TMP_ROOT/raw-stage.out" 2>&1; then
    echo 'dependency metadata normalization accepted a raw staging-root leak' >&2
    exit 1
fi
grep -Fq 'still contains the staging root' "$TMP_ROOT/raw-stage.out"
rm -f "$metadata_prefix/lib/pkgconfig/leak.cmake"
printf 'text-before\0text-after\n' >"$metadata_prefix/lib/pkgconfig/binary.cmake"
if normalize_dependency_metadata "$metadata_prefix" "$metadata_final" "$metadata_final" \
        >"$TMP_ROOT/binary-metadata.out" 2>&1; then
    echo 'dependency metadata normalization accepted a binary metadata file' >&2
    exit 1
fi
grep -Fq 'is not a text file' "$TMP_ROOT/binary-metadata.out"
rm -f "$metadata_prefix/lib/pkgconfig/binary.cmake"
CUP_DEPS_STAGE_ROOT=

first_id=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-id)
second_id=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-id)
[ "$first_id" = "$second_id" ] || {
    echo 'dependency identity is not deterministic' >&2
    exit 1
}
case "$first_id" in
    *[!0-9a-f]*)
        echo 'dependency identity is not hexadecimal' >&2
        exit 1
        ;;
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
        https://*)
            ;;
        *)
            echo "dependency source is not HTTPS: $package" >&2
            exit 1
            ;;
    esac
done

printf '%s\n' 'Dependency path-neutralization, identity and transport tests passed.'

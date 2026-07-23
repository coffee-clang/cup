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

lock_root="$TMP_ROOT/build-lock"
dependency_acquire_build_lock "$lock_root"
[ -f "$lock_root/.cup-dependencies.lock/owner" ] || {
    echo 'dependency build lock did not record its owner' >&2
    exit 1
}
if (dependency_acquire_build_lock "$lock_root") >"$TMP_ROOT/lock.out" 2>&1; then
    echo 'dependency build lock accepted a concurrent owner' >&2
    exit 1
fi
grep -Fq 'another dependency build is active' "$TMP_ROOT/lock.out"
dependency_release_build_lock
[ ! -e "$lock_root/.cup-dependencies.lock" ] || {
    echo 'dependency build lock was not released' >&2
    exit 1
}

# Do not steal a lock while its creator is between mkdir and owner creation.
sleep 5 &
delayed_owner=$!
mkdir -p "$lock_root/.cup-dependencies.lock"
(
    sleep 0.1
    printf '%s\n' "$delayed_owner" > "$lock_root/.cup-dependencies.lock/owner"
) &
owner_writer=$!
if dependency_acquire_build_lock "$lock_root"         >"$TMP_ROOT/delayed-lock.out" 2>&1; then
    echo 'dependency build lock stole a lock before its owner was recorded' >&2
    exit 1
fi
wait "$owner_writer"
grep -Fq 'another dependency build is active' "$TMP_ROOT/delayed-lock.out"
kill "$delayed_owner" 2>/dev/null || true
wait "$delayed_owner" 2>/dev/null || true
rm -rf -- "$lock_root/.cup-dependencies.lock"

mkdir -p "$lock_root/.cup-dependencies.lock"
printf '%s\n' stale > "$lock_root/.cup-dependencies.lock/owner"
dependency_acquire_build_lock "$lock_root"
[ "$(cat "$lock_root/.cup-dependencies.lock/owner")" = "$$" ] || {
    echo 'stale dependency build lock was not recovered' >&2
    exit 1
}
dependency_release_build_lock

first_key=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
second_key=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$first_key" = "$second_key" ] || {
    echo 'dependency cache key is not deterministic' >&2
    exit 1
}
case "$first_key" in
    cup-deps-linux-x64-gcc-r1-[0-9a-f]*) ;;
    *)
        echo "unexpected dependency cache key: $first_key" >&2
        exit 1
        ;;
esac

# Comments and ordering in the data lock do not change semantic compatibility.
lock_copy="$TMP_ROOT/dependencies.lock"
recipe_copy="$TMP_ROOT/dependencies.recipe"
cp "$ROOT/config/dependencies.lock" "$lock_copy"
cp "$ROOT/config/dependencies.recipe" "$recipe_copy"
commented_key=$(CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
    CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
printf '%s\n' '# transport note only' >> "$lock_copy"
reordered="$TMP_ROOT/dependencies-reordered.lock"
{
    sed -n '1p' "$lock_copy"
    sed -n '2,$p' "$lock_copy" | grep -v '^#' | sort
    printf '%s\n' '# transport note only'
} > "$reordered"
formatted_key=$(CUP_DEPENDENCY_LOCK_FILE="$reordered" \
    CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$commented_key" = "$formatted_key" ] || {
    echo 'dependency cache key changes for lock comments or ordering' >&2
    exit 1
}

sed 's/^zlib.version=.*/zlib.version=1.3.2 invalid/' "$reordered" > "$lock_copy"
if CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
        CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy" \
        "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key \
        >"$TMP_ROOT/lock-whitespace.out" 2>&1; then
    echo 'dependency lock accepted whitespace in a value' >&2
    exit 1
fi
grep -Fq 'must not contain whitespace' "$TMP_ROOT/lock-whitespace.out"

sed 's|^zlib.version=.*|zlib.version=../escape|' "$reordered" > "$lock_copy"
if CUP_DEPENDENCY_LOCK_FILE="$lock_copy"         CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy"         "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key         >"$TMP_ROOT/lock-version.out" 2>&1; then
    echo 'dependency lock accepted an unsafe version' >&2
    exit 1
fi
grep -Fq 'invalid ZLIB.version' "$TMP_ROOT/lock-version.out"

printf '1\n2\n' > "$recipe_copy"
if CUP_DEPENDENCY_LOCK_FILE="$reordered"         CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy"         "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key         >"$TMP_ROOT/recipe-lines.out" 2>&1; then
    echo 'dependency recipe accepted multiple lines' >&2
    exit 1
fi
grep -Fq 'one positive integer' "$TMP_ROOT/recipe-lines.out"
printf '1\n' > "$recipe_copy"

sed 's/^zlib.version=.*/zlib.version=9.9.9/' "$reordered" > "$lock_copy"
changed_lock_key=$(CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
    CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$changed_lock_key" != "$formatted_key" ] || {
    echo 'dependency source version did not invalidate the cache key' >&2
    exit 1
}
printf '%s\n' 2 > "$recipe_copy"
changed_recipe_key=$(CUP_DEPENDENCY_LOCK_FILE="$reordered" \
    CUP_DEPENDENCY_RECIPE_FILE="$recipe_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$changed_recipe_key" != "$formatted_key" ] || {
    echo 'dependency recipe version did not invalidate the cache key' >&2
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

printf '%s\n' 'Dependency path-neutralization, cache and transport tests passed.'

# Windows libarchive must not discover the runner's unpinned libiconv package.
grep -Fq -- '--without-iconv' "$ROOT/scripts/dependencies/build-windows.sh" || {
    echo 'Windows libarchive does not disable unpinned iconv discovery' >&2
    exit 1
}
grep -Fq -- 'unpinned libiconv' "$ROOT/scripts/dependencies/build-windows.sh" || {
    echo 'Windows dependency verification does not reject libiconv metadata' >&2
    exit 1
}

#!/usr/bin/env bash
# Verifies dependency identity, transactions, build normalization and compiled-archive guards.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/cup-dependency-contract.XXXXXX")
cleanup() {
    [ ! -e "$TMP_ROOT" ] && [ ! -L "$TMP_ROOT" ] || rm -rf -- "$TMP_ROOT"
}
exit_handler() {
    status=$?
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
signal_handler() {
    status=$1
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
trap exit_handler EXIT
trap 'signal_handler 129' HUP
trap 'signal_handler 130' INT
trap 'signal_handler 143' TERM
# shellcheck source=../../scripts/dependencies/common.sh
source "$ROOT/scripts/dependencies/common.sh"

fail() {
    printf 'Dependency contract test failed: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    case "$1" in
        *"$2"*) return 0 ;;
        *) printf 'Expected output to contain: %s\n' "$2" >&2; return 1 ;;
    esac
}


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
rm -rf -- "$metadata_stage"

lock_root="$TMP_ROOT/build-lock"
lock_path=$(dependency_lock_path "$lock_root")
dependency_acquire_build_lock "$lock_root"
[ -f "$lock_path/owner" ] || {
    echo 'dependency build lock did not record its owner' >&2
    exit 1
}
if (dependency_acquire_build_lock "$lock_root") >"$TMP_ROOT/lock.out" 2>&1; then
    echo 'dependency build lock accepted a concurrent owner' >&2
    exit 1
fi
assert_contains "$(cat "$TMP_ROOT/lock.out")" \
    'another dependency operation is active'
dependency_release_build_lock
[ ! -e "$lock_path" ] || {
    echo 'dependency build lock was not released' >&2
    exit 1
}

# An ownerless lock is the deterministic state observed while its creator is
# between directory creation and owner publication. Acquisition must fail and
# preserve the incomplete lock without depending on scheduler timing.
mkdir -p "$lock_path"
if dependency_acquire_build_lock "$lock_root" \
        >"$TMP_ROOT/ownerless-lock.out" 2>&1; then
    fail 'dependency lock acquisition stole an ownerless lock'
fi
[ -d "$lock_path" ] || fail 'ownerless dependency lock was removed'
[ ! -e "$lock_path/owner" ] && [ ! -L "$lock_path/owner" ] ||
    fail 'ownerless dependency lock received an owner'
assert_contains "$(cat "$TMP_ROOT/ownerless-lock.out")" \
    'ambiguous and was preserved'
printf '%s\n' malformed > "$lock_path/owner"
if dependency_acquire_build_lock "$lock_root" >"$TMP_ROOT/malformed-lock.out" 2>&1; then
    echo 'dependency build lock stole a malformed lock' >&2
    exit 1
fi
[ "$(cat "$lock_path/owner")" = malformed ] || fail 'malformed lock owner was replaced'
assert_contains "$(cat "$TMP_ROOT/malformed-lock.out")" \
    'invalid owner and was preserved'
rm -f -- "$lock_path/owner"
rmdir -- "$lock_path"

sleep 0.01 &
dead_owner=$!
wait "$dead_owner"
mkdir -p "$lock_path"
printf '%s\n' "$dead_owner" > "$lock_path/owner"
dependency_acquire_build_lock "$lock_root"
[ "$(cat "$lock_path/owner")" = "$$" ] || {
    echo 'stale dependency build lock was not recovered' >&2
    exit 1
}
dependency_release_build_lock

# Dependency roots reject symlinks in any existing parent component before
# creating the root marker, lock, staging or source directories.
external_dependency_parent=$TMP_ROOT/external-dependency-parent
linked_dependency_parent=$TMP_ROOT/linked-dependency-parent
mkdir -p "$external_dependency_parent"
ln -s "$external_dependency_parent" "$linked_dependency_parent"
unsafe_dependency_root=$linked_dependency_parent/nested/dependencies
if dependency_prepare_root "$unsafe_dependency_root" \
        >"$TMP_ROOT/dependency-parent-link.out" 2>&1; then
    fail 'dependency root followed a symlinked parent'
fi
grep -Fq 'symlink or reparse point' "$TMP_ROOT/dependency-parent-link.out"
[ ! -e "$external_dependency_parent/nested" ] ||
    fail 'dependency root created an external directory through a symlink'

# A source-cache hit must be a regular file owned through the dependency path
# chain. Correct bytes behind a symlink must not bypass that ownership check.
cache_root=$TMP_ROOT/cache-root
cache_external=$TMP_ROOT/cache-external.tar.gz
mkdir -p "$cache_root/src"
dd if=/dev/zero of="$cache_external" bs=1024 count=128 >/dev/null 2>&1
ln -s "$cache_external" "$cache_root/src/zlib.tar.gz"
if (
    DEPS_ROOT=$cache_root
    verify_source_checksum() { return 0; }
    download_source zlib "$cache_root/src/zlib.tar.gz"
) >"$TMP_ROOT/cache-symlink.out" 2>&1; then
    fail 'dependency source cache accepted a symlink'
fi
grep -Fq 'cached dependency archive is not a safe regular file' "$TMP_ROOT/cache-symlink.out" ||
    fail 'dependency source cache did not diagnose the unsafe cache entry'
[ -L "$cache_root/src/zlib.tar.gz" ] || fail 'dependency source cache modified the symlink'
[ -f "$cache_external" ] || fail 'dependency source cache modified the external target'

# Download transport limits are part of the dependency contract even when the
# source checksum is valid.
download_root=$TMP_ROOT/download-root
download_temp=$TMP_ROOT/download-temp
download_temp_alias=$TMP_ROOT/download-temp-alias
mkdir -p "$download_root/src" "$download_temp"
ln -s "$download_temp" "$download_temp_alias"
(
    DEPS_ROOT=$download_root
    verify_source_checksum() { return 0; }
    fake_download_bin=$TMP_ROOT/download-bin
    mkdir -p "$fake_download_bin"
    cat > "$fake_download_bin/curl" <<'EOF_DOWNLOAD_CURL'
#!/bin/sh
set -eu
printf '%s\n' "$@" > "$CUP_DOWNLOAD_ARGUMENTS"
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) output=$2; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$output" ]
dd if=/dev/zero of="$output" bs=1024 count=128 >/dev/null 2>&1
EOF_DOWNLOAD_CURL
    chmod +x "$fake_download_bin/curl"
    TMPDIR="$download_temp_alias/" PATH="$fake_download_bin:$PATH" \
        CUP_DOWNLOAD_ARGUMENTS="$TMP_ROOT/download-arguments" \
        download_source zlib "$download_root/src/zlib.tar.gz"
)
for required_option in --connect-timeout --max-time --speed-limit \
        --speed-time --max-filesize; do
    grep -Fx -- "$required_option" "$TMP_ROOT/download-arguments" >/dev/null ||
        fail "dependency download omitted $required_option"
done

first_key=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
second_key=$("$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$first_key" = "$second_key" ] || {
    echo 'dependency cache key is not deterministic' >&2
    exit 1
}
if [[ ! "$first_key" =~ ^cup-deps-f5-linux-x64-gcc-b4-[0-9a-f]{64}-[0-9a-f]{64}$ ]]; then
    echo "unexpected dependency cache key: $first_key" >&2
    exit 1
fi

# Comments and ordering in the data lock do not change semantic compatibility.
lock_copy="$TMP_ROOT/dependencies.lock"
cp "$ROOT/config/dependencies.lock" "$lock_copy"
commented_key=$(CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
printf '%s\n' '# transport note only' >> "$lock_copy"
reordered="$TMP_ROOT/dependencies-reordered.lock"
{
    grep -v '^#' "$lock_copy" | sort
    printf '%s\n' '# transport note only'
} > "$reordered"
formatted_key=$(CUP_DEPENDENCY_LOCK_FILE="$reordered" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$commented_key" = "$formatted_key" ] || {
    echo 'dependency cache key changes for lock comments or ordering' >&2
    exit 1
}

sed 's/^zlib.version=.*/zlib.version=1.3.2 invalid/' "$reordered" > "$lock_copy"
if CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
        "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key \
        >"$TMP_ROOT/lock-whitespace.out" 2>&1; then
    echo 'dependency lock accepted whitespace in a value' >&2
    exit 1
fi
grep -Fq 'must not contain whitespace' "$TMP_ROOT/lock-whitespace.out"

sed 's|^zlib.version=.*|zlib.version=../escape|' "$reordered" > "$lock_copy"
if CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
        "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key \
        >"$TMP_ROOT/lock-version.out" 2>&1; then
    echo 'dependency lock accepted an unsafe version' >&2
    exit 1
fi
grep -Fq 'invalid zlib.version' "$TMP_ROOT/lock-version.out"

sed 's/^build_revision=.*/build_revision=1 2/' "$reordered" > "$lock_copy"
if CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
        "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key \
        >"$TMP_ROOT/build-revision.out" 2>&1; then
    echo 'dependency lock accepted an invalid build revision' >&2
    exit 1
fi
grep -Fq 'must not contain whitespace' "$TMP_ROOT/build-revision.out"

sed 's/^zlib.version=.*/zlib.version=9.9.9/' "$reordered" > "$lock_copy"
changed_lock_key=$(CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$changed_lock_key" != "$formatted_key" ] || {
    echo 'dependency source version did not invalidate the cache key' >&2
    exit 1
}
sed 's/^build_revision=.*/build_revision=999/' "$reordered" > "$lock_copy"
changed_revision_key=$(CUP_DEPENDENCY_LOCK_FILE="$lock_copy" \
    "$ROOT/scripts/dependencies/verify.sh" linux-x64 --print-cache-key)
[ "$changed_revision_key" != "$formatted_key" ] || {
    echo 'dependency build revision did not invalidate the cache key' >&2
    exit 1
}

grep -Fq 'no-apps no-docs no-autoload-config no-dso' \
    "$ROOT/scripts/dependencies/build-posix.sh" || {
    echo 'OpenSSL builder does not limit output to consumed development artifacts' >&2
    exit 1
}
grep -Fq 'make -j"$JOBS" build_libs' \
    "$ROOT/scripts/dependencies/build-posix.sh" || {
    echo 'OpenSSL builder does not build only static libraries' >&2
    exit 1
}
grep -Fq 'make install_dev DESTDIR="$install_root"' \
    "$ROOT/scripts/dependencies/build-posix.sh" || {
    echo 'OpenSSL builder does not install only development artifacts' >&2
    exit 1
}
if grep -Fq 'make install_sw DESTDIR="$install_root"' \
        "$ROOT/scripts/dependencies/build-posix.sh"; then
    echo 'OpenSSL builder still installs runtime programs or modules' >&2
    exit 1
fi

for package in zlib xz openssl cares curl libarchive argtable3 uthash unity libevent; do
    case "$(source_url_for_package "$package")" in
        https://*)
            ;;
        *)
            echo "dependency source is not HTTPS: $package" >&2
            exit 1
            ;;
    esac
done


printf '==> Testing dependency-prefix transactions...\n'
DEPENDENCY_COMMON="$ROOT/scripts/dependencies/common.sh"
TRANSACTION_ROOT="$TMP_ROOT/transaction-root"
TRANSACTION_PREFIX="$TRANSACTION_ROOT/install"
mkdir -p "$TRANSACTION_ROOT"
dependency_write_root_marker "$TRANSACTION_ROOT"

bash -eu -o pipefail -c '
    common=$1
    final=$2
    verifier=$3
    version=$4
    . "$common"
    DEPS_ROOT=${final%/install}

    create_complete() {
        prefix=$1
        embedded_prefix=$2
        mkdir -p "$prefix/bin" "$prefix/include/curl" \
            "$prefix/include/openssl" "$prefix/include/event2" \
            "$prefix/lib/pkgconfig"
        for header in \
            argtable3.h uthash.h ares.h unity.h unity_internals.h \
            event2/event.h event2/http.h event2/bufferevent.h \
            event2/listener.h curl/curl.h archive.h archive_entry.h \
            zlib.h lzma.h openssl/ssl.h; do
            printf "/* dependency fixture */\n" > "$prefix/include/$header"
        done
        for archive in \
            libargtable3.a libcares.a libunity.a libevent_core.a \
            libevent_extra.a libcurl.a libarchive.a libz.a liblzma.a \
            libssl.a libcrypto.a; do
            ar rcs "$prefix/lib/$archive"
        done
        cat >"$prefix/bin/curl-config" <<EOF_CURL_CONFIG
#!/bin/sh
case "\${1:-}" in
    --features) printf "%s\n" AsynchDNS ;;
    --static-libs|"") printf "%s\n" "-L$embedded_prefix/lib -lcurl -lcares" ;;
    --configure)
        printf " \047--prefix=$embedded_prefix\047"
        printf " \047--enable-ares=$embedded_prefix\047\n"
        ;;
    *) exit 2 ;;
esac
EOF_CURL_CONFIG
        chmod +x "$prefix/bin/curl-config"
        cat >"$prefix/lib/pkgconfig/libcares.pc" <<EOF_CARES_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: c-ares
Description: test metadata
Version: 1
Libs: -L\${libdir} -lcares
EOF_CARES_PC
        cat >"$prefix/lib/pkgconfig/libarchive.pc" <<EOF_LIBARCHIVE_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libarchive
Description: test metadata
Version: 1
Libs: -L\${libdir} -larchive
EOF_LIBARCHIVE_PC
        cat >"$prefix/lib/pkgconfig/libevent_core.pc" <<EOF_EVENT_CORE_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libevent_core
Description: test metadata
Version: 1
Libs: -L\${libdir} -levent_core
EOF_EVENT_CORE_PC
        cat >"$prefix/lib/pkgconfig/libevent_extra.pc" <<EOF_EVENT_EXTRA_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libevent_extra
Description: test metadata
Version: 1
Requires.private: libevent_core
Libs: -L\${libdir} -levent_extra
EOF_EVENT_EXTRA_PC
    }

    source_lock_sha256=$(dependency_lock_sha256)
    toolchain_sha256=$(dependency_toolchain_sha256 linux-x64 gcc)
    metadata=$(dependency_metadata linux-x64 gcc)
    [ "$metadata" = "prefix_format=5
product=coffee-clang/cup
kind=dependency-prefix
platform=linux-x64
profile=gcc
build_revision=$DEPENDENCY_BUILD_REVISION
source_lock_sha256=$source_lock_sha256
toolchain_sha256=$toolchain_sha256" ]
    dependency_metadata_valid "$metadata"

    foreign_root="${final%/install}.foreign"
    mkdir -p "$foreign_root/install"
    cat > "$foreign_root/install/.cup-dependencies" <<'EOF_FOREIGN_PREFIX'
prefix_format=5
platform=linux-x64
profile=gcc
owner=foreign
EOF_FOREIGN_PREFIX
    printf '%s\n' external > "$foreign_root/install/sentinel.txt"
    if dependency_prefix_owned "$foreign_root/install"; then
        fail "partial foreign dependency metadata was accepted as cup ownership"
    fi
    if dependency_prepare_root "$foreign_root"; then
        fail "dependency root adopted a foreign prefix with partial metadata"
    fi
    [ -f "$foreign_root/install/sentinel.txt" ] ||
        fail "foreign dependency prefix was modified during rejected adoption"
    [ ! -e "$foreign_root/.cup-dependencies-root" ] ||
        fail "foreign dependency root received a cup ownership marker"

    if dependency_metadata_valid "prefix_format=4
product=coffee-clang/cup
kind=dependency-prefix
platform=linux-x64
profile=gcc
build_revision=$DEPENDENCY_BUILD_REVISION
source_lock_sha256=$source_lock_sha256
toolchain_sha256=$toolchain_sha256"; then
        exit 1
    fi
    if dependency_metadata_valid "prefix_format=5
product=coffee-clang/cup
kind=dependency-prefix
platform=linux-x64
profile=apple-clang
build_revision=$DEPENDENCY_BUILD_REVISION
source_lock_sha256=$source_lock_sha256
toolchain_sha256=$toolchain_sha256"; then
        exit 1
    fi

    if prepare_dependency_prefix relative-prefix "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix / "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/install/ "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/../escape "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/./install "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix "/tmp/with space/install" "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/install "$metadata" 2; then
        exit 1
    fi
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ -n "$CUP_DEPS_STAGE_ROOT" ]
    [ "$CUP_DEPS_BUILD_PREFIX" = "$CUP_DEPS_STAGE_ROOT$final" ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    create_complete "$CUP_DEPS_BUILD_PREFIX" "$CUP_DEPS_BUILD_PREFIX"
    printf "new\n" >"$CUP_DEPS_BUILD_PREFIX/new.txt"

    cygpath() {
        case "$1" in
            -m)
                printf "D:/msys64%s\n" "$2"
                ;;
            -w)
                converted=$(printf "%s" "$2" | sed "s#/#\\\\#g")
                printf "D:\\msys64%s\n" "$converted"
                ;;
            *)
                return 1
                ;;
        esac
    }
    staged_native=$(cygpath -m "$CUP_DEPS_BUILD_PREFIX")
    final_native=$(cygpath -m "$final")
    staged_windows=$(cygpath -w "$CUP_DEPS_BUILD_PREFIX")
    final_windows=$(cygpath -w "$final")
    mixed_first=${staged_native%%/*}
    mixed_rest=${staged_native#*/}
    mixed_second=${mixed_rest%%/*}
    mixed_rest=${mixed_rest#*/}
    mixed_third=${mixed_rest%%/*}
    mixed_rest=${mixed_rest#*/}
    if [ "$mixed_rest" = "$mixed_third" ]; then
        echo "Error: could not construct a mixed-separator test path." >&2
        exit 1
    fi
    mixed_native="$mixed_first/$mixed_second/$mixed_third\\$mixed_rest"
    cat >"$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig/windows-paths.cmake" <<EOF_WINDOWS_PATHS
posix=$CUP_DEPS_BUILD_PREFIX
native=$staged_native
windows=$staged_windows
mixed=$mixed_native
EOF_WINDOWS_PATHS
    normalize_dependency_metadata "$CUP_DEPS_BUILD_PREFIX" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"
    ! find "$CUP_DEPS_BUILD_PREFIX" -type f \
        \( -name '*.pc' -o -name '*.la' -o -name '*.cmake' \
           -o -name '*-config' -o -name 'curl-config' \) \
        -exec grep -F -l "$CUP_DEPS_STAGE_ROOT" {} + | grep .
    [ "$("$CUP_DEPS_BUILD_PREFIX/bin/curl-config")" = "-L$final/lib -lcurl -lcares" ]
    windows_metadata="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig/windows-paths.cmake"
    grep -F "posix=$final" "$windows_metadata" >/dev/null
    grep -F "native=$final_native" "$windows_metadata" >/dev/null
    grep -F "windows=$final_native" "$windows_metadata" >/dev/null
    grep -F "mixed=$final_native" "$windows_metadata" >/dev/null
    ! grep -F "$CUP_DEPS_BUILD_PREFIX" "$windows_metadata" >/dev/null
    ! grep -F "$staged_native" "$windows_metadata" >/dev/null
    ! grep -F "$staged_windows" "$windows_metadata" >/dev/null
    relocated_archive_flags=$( \
        PKG_CONFIG_PATH="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_LIBDIR="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="" \
        pkg-config --define-prefix --libs libarchive)
    case "$relocated_archive_flags" in
        *"$CUP_DEPS_BUILD_PREFIX"*) ;;
        *)
            echo "Error: pkg-config relocation test did not expose the staging prefix." >&2
            exit 1
            ;;
    esac
    archive_flags=$(PKG_CONFIG_PATH="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_LIBDIR="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --libs libarchive)
    [ "$archive_flags" = "-L$final/lib -larchive " ] || \
        [ "$archive_flags" = "-L$final/lib -larchive" ]
    dependency_link_flags_valid "-L$final_native/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"
    dependency_link_flags_valid "-Wl,-L,$final_native/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"
    if dependency_link_flags_valid \
        "-L$final/lib -L$staged_native/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "native staging path was accepted in dependency link metadata"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -Wl,-L,/usr/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "host linker search path was accepted in dependency metadata"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -L$final/lib/../../outside -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "private-prefix traversal was accepted in dependency metadata"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -Wl,-L,$final\\lib\\..\\..\\outside -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "mixed-separator private-prefix traversal was accepted"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -Wl,-rpath,/usr/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "host runtime search path was accepted in dependency metadata"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -Wl,-R,/usr/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "host -R runtime path was accepted in dependency metadata"
    fi
    if dependency_link_flags_valid \
        "-L$final/lib -R/usr/lib -lcurl" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "direct host -R path was accepted in dependency metadata"
    fi
    if dependency_link_flags_valid "-L" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"; then
        fail "incomplete linker search flag was accepted in dependency metadata"
    fi
    finish_dependency_prefix "$CUP_DEPS_BUILD_PREFIX"
    [ -f "$final/new.txt" ]
    [ ! -e "$final/.cup-deps-building" ]
    [ "$(cat "$final/.cup-dependencies")" = "$metadata" ]
    [ "$("$final/bin/curl-config")" = "-L$final/lib -lcurl -lcares" ]

    evidence=$DEPS_ROOT/evidence.txt
    cache_key=$(dependency_cache_key linux-x64 gcc)
    repository=example/cup
    commit=0123456789abcdef0123456789abcdef01234567
    run_id=31
    run_attempt=2
    artifact=cup-dependency-evidence-linux-x64-gcc-attempt-2
    {
        printf "%s\n" format=2 "version=$version" \
            "source_repository=$repository" "source_commit=$commit" \
            "run_id=$run_id" "run_attempt=$run_attempt" \
            "artifact_name=$artifact" target=linux-x64-gcc platform=linux-x64 \
            profile=gcc "cache_key=$cache_key"
        printf "%s\n" "$metadata"
    } >"$evidence"
    CUP_DEPENDENCY_PROFILE=gcc "$verifier" \
        "$evidence" linux-x64-gcc linux-x64 gcc "$final" \
        "$repository" "$commit" "$run_id" "$run_attempt" "$artifact" >/dev/null

    head -c -1 "$evidence" >"$evidence.invalid"
    printf "\0\n" >>"$evidence.invalid"
    if CUP_DEPENDENCY_PROFILE=gcc "$verifier" \
            "$evidence.invalid" linux-x64-gcc linux-x64 gcc "$final" \
            "$repository" "$commit" "$run_id" "$run_attempt" "$artifact" \
            >"$DEPS_ROOT/evidence-nul.out" 2>&1; then
        fail "dependency evidence accepted a hidden NUL byte"
    fi
    grep -Fq "non-canonical bytes" "$DEPS_ROOT/evidence-nul.out"

    head -c -1 "$evidence" >"$evidence.invalid"
    if CUP_DEPENDENCY_PROFILE=gcc "$verifier" \
            "$evidence.invalid" linux-x64-gcc linux-x64 gcc "$final" \
            "$repository" "$commit" "$run_id" "$run_attempt" "$artifact" \
            >"$DEPS_ROOT/evidence-lf.out" 2>&1; then
        fail "dependency evidence accepted a missing final LF"
    fi
    grep -Fq "not LF-terminated" "$DEPS_ROOT/evidence-lf.out"
    rm -f "$evidence" "$evidence.invalid" \
        "$DEPS_ROOT/evidence-nul.out" "$DEPS_ROOT/evidence-lf.out"

    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 1 ]
    [ "$CUP_DEPS_BUILD_PREFIX" = "$final" ]

    cp "$final/bin/curl-config" "$final/bin/curl-config.valid"
    printf "#!/bin/sh\nexit 0\n" >"$final/bin/curl-config"
    chmod +x "$final/bin/curl-config"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/bin/curl-config.valid" "$final/bin/curl-config"

    cp "$final/bin/curl-config" "$final/bin/curl-config.valid"
    cat >"$final/bin/curl-config" <<EOF_NO_CARES_CONFIGURE
#!/bin/sh
case "\${1:-}" in
    --static-libs) printf "%s\n" "-L$final/lib -lcurl -lcares" ;;
    --features) printf "%s\n" AsynchDNS ;;
    --configure) printf " \047--prefix=$final\047\n" ;;
    *) exit 2 ;;
esac
EOF_NO_CARES_CONFIGURE
    chmod +x "$final/bin/curl-config"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/bin/curl-config.valid" "$final/bin/curl-config"

    cp "$final/lib/pkgconfig/libarchive.pc" \
        "$final/lib/pkgconfig/libarchive.pc.valid"
    printf "Libs.private: -lacl\n" >> \
        "$final/lib/pkgconfig/libarchive.pc"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/lib/pkgconfig/libarchive.pc.valid" \
        "$final/lib/pkgconfig/libarchive.pc"

    cp "$final/bin/curl-config" "$final/bin/curl-config.valid"
    cat >"$final/bin/curl-config" <<EOF_HOST_SEARCH
#!/bin/sh
printf "%s\\n" "-L$final/lib -lcurl -L/usr/lib -lhost"
EOF_HOST_SEARCH
    chmod +x "$final/bin/curl-config"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/bin/curl-config.valid" "$final/bin/curl-config"

    cp "$final/lib/pkgconfig/libarchive.pc" \
        "$final/lib/pkgconfig/libarchive.pc.valid"
    printf "Libs.private: /usr/lib/libhost.a\\n" >> \
        "$final/lib/pkgconfig/libarchive.pc"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/lib/pkgconfig/libarchive.pc.valid" \
        "$final/lib/pkgconfig/libarchive.pc"

    cp "$final/lib/pkgconfig/libarchive.pc" \
        "$final/lib/pkgconfig/libarchive.pc.valid"
    printf "Libs.private: -ldl -lpthread\\n" >> \
        "$final/lib/pkgconfig/libarchive.pc"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 1 ]
    [ "$CUP_DEPS_BUILD_PREFIX" = "$final" ]
    mv "$final/lib/pkgconfig/libarchive.pc.valid" \
        "$final/lib/pkgconfig/libarchive.pc"

    rm "$final/include/uthash.h"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    [ -f "$final/new.txt" ]
    abort_dependency_prefix
' sh "$DEPENDENCY_COMMON" "$TRANSACTION_PREFIX" \
    "$ROOT/scripts/ci/verify-dependency-evidence.sh" "$(cat "$ROOT/VERSION")"

ZLIB_VERSION=0 ZLIB_URL=https://invalid.example/zlib.tar.gz bash -eu -c '
    . "$1"
    [ "$ZLIB_VERSION" = 1.3.2 ]
    case "$ZLIB_URL" in
        https://github.com/madler/zlib/*) ;;
        *)
            exit 1
            ;;
    esac
' sh "$DEPENDENCY_COMMON"

FAILED_ROOT="$TMP_ROOT/failed-root"
FAILED_PREFIX="$FAILED_ROOT/install"
bash -eu -o pipefail -c '
    common=$1
    final=$2
    . "$common"
    DEPS_ROOT=${final%/install}

    metadata=$(dependency_metadata linux-x64 gcc)
    prepare_dependency_prefix "$final" "$metadata" 1
    build=$CUP_DEPS_BUILD_PREFIX
    printf "partial\n" >"$build/partial.txt"
    if finish_dependency_prefix "$build"; then
        exit 1
    fi
    [ ! -e "$final" ]
    [ -d "$build" ]
    abort_dependency_prefix
    [ ! -e "$build" ]
    [ ! -e "$final" ]
    [ -z "$CUP_DEPS_STAGE_ROOT" ]
    [ -z "$CUP_DEPS_BUILD_PREFIX" ]
    [ -z "$CUP_DEPS_FINAL_PREFIX" ]
' sh "$DEPENDENCY_COMMON" "$FAILED_PREFIX"

leftovers=$(find "$TMP_ROOT" -name '.install.staging' -print)
[ -z "$leftovers" ] || fail "dependency staging directories were not cleaned: $leftovers"
printf 'Dependency-prefix transaction tests passed.\n'

printf '==> Testing failed dependency build cleanup and retry...\n'
FAILED_DEPENDENCY_ROOT="$TMP_ROOT/failed-dependency-build"
FAILED_DEPENDENCY_PREFIX="$FAILED_DEPENDENCY_ROOT/install"
FAKE_CURL_DIR="$TMP_ROOT/failing-curl"
mkdir -p "$FAKE_CURL_DIR"
cat >"$FAKE_CURL_DIR/curl" <<'EOF_CURL'
#!/bin/sh
exit 7
EOF_CURL
chmod +x "$FAKE_CURL_DIR/curl"

attempt=1
while [ "$attempt" -le 2 ]; do
    if (
        cd "$ROOT"
        PATH="$FAKE_CURL_DIR:$PATH" \
            DEPS_ROOT="$FAILED_DEPENDENCY_ROOT" \
            DEPS_PREFIX="$FAILED_DEPENDENCY_PREFIX" \
            PLATFORM=linux-x64 \
            bash ./scripts/dependencies/build-posix.sh
    ) >"$TMP_ROOT/failed-dependency-build-$attempt.out" 2>&1; then
        fail 'dependency build unexpectedly succeeded with a failing downloader'
    fi
    [ ! -e "$FAILED_DEPENDENCY_PREFIX" ] ||
        fail 'failed dependency build exposed a partial final prefix'
    leftovers=$(find "$FAILED_DEPENDENCY_ROOT" -maxdepth 1 \
        -name '.install.staging' -print 2>/dev/null || true)
    [ -z "$leftovers" ] ||
        fail "failed dependency build left staging directories: $leftovers"
    attempt=$((attempt + 1))
done
printf 'Failed dependency build cleanup and retry tests passed.\n'

printf '==> Testing dependency diagnostics...\n'
missing_prefix="$TMP_ROOT/missing-prefix"
(
    cd "$ROOT"
    make --no-print-directory -n PLATFORM=linux-x64 \
        DEPS_PREFIX="$missing_prefix" all
) >"$TMP_ROOT/missing-prefix.out" 2>&1 || true
if (
    cd "$ROOT"
    make --no-print-directory -s PLATFORM=linux-x64 \
        DEPS_PREFIX="$missing_prefix" deps-check
) >"$TMP_ROOT/deps-check-missing.out" 2>&1; then
    fail 'deps-check accepted a missing dependency prefix'
fi
assert_contains "$(cat "$TMP_ROOT/deps-check-missing.out")" \
    'Dependency prefix check failed: metadata file is missing, empty or not a regular file:'
assert_contains "$(cat "$TMP_ROOT/deps-check-missing.out")" \
    'Pinned dependency prefix is missing, incomplete or incompatible'

default_deps_prefix="$HOME/deps/linux-x64/install"
deps_command=$(
    cd "$ROOT"
    unset DEPS_ROOT DEPS_PREFIX MAKEFLAGS MAKEOVERRIDES
    make --no-print-directory -B -n PLATFORM=linux-x64 deps
)
assert_contains "$deps_command" "$default_deps_prefix"
custom_deps_prefix="$TMP_ROOT/custom-deps-prefix"
custom_deps_command=$(
    cd "$ROOT"
    unset DEPS_ROOT DEPS_PREFIX MAKEFLAGS MAKEOVERRIDES
    make --no-print-directory -B -n PLATFORM=linux-x64 \
        DEPS_PREFIX="$custom_deps_prefix" deps
)
assert_contains "$custom_deps_command" "DEPS_PREFIX='$custom_deps_prefix'"
printf 'Dependency diagnostic tests passed.\n'

printf '==> Testing canonical dependency identity...\n'
IDENTITY_BIN="$TMP_ROOT/identity-bin"
mkdir -p "$IDENTITY_BIN"
cat >"$IDENTITY_BIN/clang" <<'EOF_CLANG'
#!/bin/sh
case "${1:-}" in
    -dumpmachine|-print-target-triple) printf '%s\n' x86_64-apple-darwin ;;
    --version) printf '%s\n' 'Apple clang version 18.0.0' ;;
    *) exit 2 ;;
esac
EOF_CLANG
cat >"$IDENTITY_BIN/ar" <<'EOF_AR'
#!/bin/sh
[ "${1:-}" = --version ] || exit 2
printf '%s\n' 'Apple ar 1.0'
EOF_AR
cat >"$IDENTITY_BIN/ranlib" <<'EOF_RANLIB'
#!/bin/sh
[ "${1:-}" = --version ] || exit 2
printf '%s\n' 'Apple ranlib 1.0'
EOF_RANLIB
cat >"$IDENTITY_BIN/xcrun" <<'EOF_XCRUN'
#!/bin/sh
[ "$*" = '--sdk macosx --show-sdk-version' ] || exit 2
printf '%s\n' 15.0
EOF_XCRUN
chmod +x "$IDENTITY_BIN/clang" "$IDENTITY_BIN/ar" "$IDENTITY_BIN/ranlib" "$IDENTITY_BIN/xcrun"
identity_unset=$(
    PATH="$IDENTITY_BIN:$PATH" bash -eu -c '
        unset MACOSX_DEPLOYMENT_TARGET MSYSTEM
        . "$1"
        dependency_cache_key macos-x64 apple-clang
    ' sh "$DEPENDENCY_COMMON"
)
identity_expected=$(
    PATH="$IDENTITY_BIN:$PATH" MACOSX_DEPLOYMENT_TARGET=13.0 MSYSTEM=UCRT64 \
        bash -eu -c '. "$1"; dependency_cache_key macos-x64 apple-clang' \
        sh "$DEPENDENCY_COMMON"
)
identity_ambient_wrong=$(
    PATH="$IDENTITY_BIN:$PATH" MACOSX_DEPLOYMENT_TARGET=12.0 MSYSTEM=CLANG64 \
        bash -eu -c '. "$1"; dependency_cache_key macos-x64 apple-clang' \
        sh "$DEPENDENCY_COMMON"
)
[ "$identity_unset" = "$identity_expected" ] ||
    fail 'macOS dependency cache key changed with ambient deployment target/MSYSTEM'
[ "$identity_unset" = "$identity_ambient_wrong" ] ||
    fail 'macOS dependency cache key depends on non-canonical ambient state'
metadata_unset=$(
    PATH="$IDENTITY_BIN:$PATH" bash -eu -c '
        unset MACOSX_DEPLOYMENT_TARGET MSYSTEM
        . "$1"
        dependency_metadata macos-x64 apple-clang
    ' sh "$DEPENDENCY_COMMON"
)
metadata_ambient=$(
    PATH="$IDENTITY_BIN:$PATH" MACOSX_DEPLOYMENT_TARGET=12.0 MSYSTEM=UCRT64 \
        bash -eu -c '. "$1"; dependency_metadata macos-x64 apple-clang' \
        sh "$DEPENDENCY_COMMON"
)
[ "$metadata_unset" = "$metadata_ambient" ] ||
    fail 'macOS dependency metadata depends on non-canonical ambient state'
[ "$(PATH="$IDENTITY_BIN:$PATH" bash -eu -c '. "$1"; dependency_macos_deployment_target macos-x64 apple-clang' sh "$DEPENDENCY_COMMON")" = 13.0 ] ||
    fail 'canonical macOS deployment target changed unexpectedly'
printf 'Canonical dependency identity tests passed.\n'

printf '==> Testing dependency platform rejection...\n'
DEPENDENCY_DIR="$ROOT/scripts/dependencies"
FAKE_MACOS_BIN="$TMP_ROOT/fake-macos-bin"
mkdir -p "$FAKE_MACOS_BIN"
cat >"$FAKE_MACOS_BIN/uname" <<'EOF_UNAME'
#!/bin/sh
case "${1:-}" in
    -s) printf '%s\n' Darwin ;;
    -m) printf '%s\n' x86_64 ;;
    *) printf '%s\n' Darwin ;;
esac
EOF_UNAME
chmod +x "$FAKE_MACOS_BIN/uname"
if PATH="$FAKE_MACOS_BIN:$PATH" PLATFORM=macos-x64 MACOSX_DEPLOYMENT_TARGET=12.0 \
        bash "$DEPENDENCY_DIR/build-posix.sh" \
        >"$TMP_ROOT/macos-deps-floor.out" 2>&1; then
    fail 'macOS dependency builder accepted the wrong deployment target'
fi
assert_contains "$(cat "$TMP_ROOT/macos-deps-floor.out")" \
    'require MACOSX_DEPLOYMENT_TARGET=13.0'
if MSYSTEM=MINGW64 MINGW_PREFIX=/mingw64 \
        bash "$DEPENDENCY_DIR/build-windows.sh" \
        >"$TMP_ROOT/windows-deps-runtime.out" 2>&1; then
    fail 'Windows dependency builder accepted a non-UCRT64 shell'
fi
assert_contains "$(cat "$TMP_ROOT/windows-deps-runtime.out")" \
    'require an MSYS2 UCRT64 or CLANG64 shell'
printf 'Dependency platform rejection tests passed.\n'

printf '==> Testing dependency inventory, scopes and notices...\n'
DEPENDENCY_SOURCES="$DEPENDENCY_DIR/sources.sh"
DEPENDENCY_NOTICES="$DEPENDENCY_DIR/THIRD_PARTY_NOTICES.txt"
[ -f "$DEPENDENCY_NOTICES" ] || fail 'third-party notices file is missing'
packages=$(sh -eu -c \
    'CUP_DEPENDENCIES_DIR=$(dirname "$1"); . "$1"; all_source_packages' \
    sh "$DEPENDENCY_SOURCES")
expected_packages='zlib
xz
openssl
cares
curl
libarchive
argtable3
uthash
unity
libevent'
[ "$packages" = "$expected_packages" ] ||
    fail 'canonical dependency inventory changed unexpectedly'
notices_content=$(cat "$DEPENDENCY_NOTICES")
assert_contains "$notices_content" 'cup third-party notices'
assert_contains "$notices_content" 'Brad Conte crypto-algorithms SHA-256 (adapted in tree)'
assert_contains "$notices_content" 'src/third_party/sha256.c'
assert_contains "$notices_content" 'Scope: runtime'
assert_contains "$notices_content" 'Scope: test'
assert_contains "$notices_content" \
    'Usage: static libraries linked only into the test network helper'
printf 'Dependency inventory and notice tests passed.\n'

printf '==> Testing normalized dependency build environment...\n'
bash -eu -o pipefail -c '
    common=$1
    . "$common"

    CFLAGS=ambient-cflags
    CPPFLAGS=ambient-cppflags
    LDFLAGS=ambient-ldflags
    LIBS=ambient-libs
    CPATH=/ambient/include
    LIBRARY_PATH=/ambient/lib
    PKG_CONFIG_PATH=/ambient/pkgconfig
    CONFIG_SITE=/ambient/config.site
    CCACHE=ambient-ccache
    MAKEFLAGS=ambient-makeflags
    SOURCE_DATE_EPOCH=ambient-epoch
    export CFLAGS CPPFLAGS LDFLAGS LIBS CPATH LIBRARY_PATH
    export PKG_CONFIG_PATH CONFIG_SITE CCACHE MAKEFLAGS SOURCE_DATE_EPOCH

    dependency_normalize_build_environment
    [ "$LC_ALL" = C ]
    [ "$LANG" = C ]
    [ "$TZ" = UTC ]
    [ "$SOURCE_DATE_EPOCH" = 1 ]
    [ "$(umask)" = 0022 ] || [ "$(umask)" = 022 ]
    for variable in CFLAGS CPPFLAGS LDFLAGS LIBS CPATH LIBRARY_PATH \
            PKG_CONFIG_PATH CONFIG_SITE CCACHE MAKEFLAGS; do
        if [[ -v $variable ]]; then
            exit 1
        fi
    done

    unset JOBS
    [ "$(dependency_resolve_jobs)" = 4 ]
    JOBS=7
    [ "$(dependency_resolve_jobs)" = 7 ]
    JOBS=0
    if dependency_resolve_jobs >/dev/null 2>&1; then
        exit 1
    fi
    JOBS=invalid
    if dependency_resolve_jobs >/dev/null 2>&1; then
        exit 1
    fi
' sh "$DEPENDENCY_COMMON"
printf 'Normalized dependency build environment tests passed.\n'

for builder in scripts/dependencies/build-posix.sh scripts/dependencies/build-windows.sh; do
    for option in --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc; do
        grep -F -- "$option" "$builder" >/dev/null || {
            echo "$builder does not restrict XZ to the consumed liblzma payload ($option missing)" >&2
            exit 1
        }
    done
done

printf '%s\n' 'Dependency contract tests passed.'

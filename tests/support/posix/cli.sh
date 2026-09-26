
# Provides isolated cup homes, catalogs, package fixtures and generated wrappers to POSIX integration tests.
# This file is sourced, not executed.

: "${TESTS_ROOT:?TESTS_ROOT must be set before sourcing tests/support/posix/cli.sh}"
. "$TESTS_ROOT/support/common.sh"

. "$TESTS_ROOT/support/environment.sh"
cup_test_prepare_environment
TEST_PLATFORM=$CUP_TEST_PLATFORM
TEST_CONFIGURATION=${CUP_TEST_CONFIGURATION:-development}
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
TEST_BINARY=${CUP_TEST_BINARY:-$TEST_BUILD_ROOT/$TEST_PLATFORM/$TEST_CONFIGURATION/bin/cup}
export PROJECT_ROOT TEST_PLATFORM TEST_CONFIGURATION TEST_BUILD_ROOT TEST_BINARY

prepare_command_environment() {
    for variable in \
        CUP_INSTALL_BASE_URL CUP_INSTALL_ALLOW_INSECURE \
        HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY \
        http_proxy https_proxy all_proxy no_proxy; do
        unset "$variable"
    done

    TEST_HOME=$TMP_ROOT/home
    DEV_ROOT=$TMP_ROOT/development-root
    CUP=$TEST_BINARY
    export TEST_HOME DEV_ROOT CUP

    assert_file "$CUP"
    mkdir -p "$TEST_HOME" "$DEV_ROOT/config"
    cp "$PROJECT_ROOT/tests/fixtures/catalog.cfg" "$DEV_ROOT/config/catalog.cfg"

    if [ "${TEST_PLATFORM%%-*}" = windows ]; then
        fail 'POSIX command environment cannot target Windows'
    fi

}

run_cup() {
    (cd "$DEV_ROOT" && HOME="$TEST_HOME" "$CUP" "$@")
}

run_cup_with_managed_path() {
    (cd "$DEV_ROOT" && HOME="$TEST_HOME" PATH="$TEST_HOME/.cup/bin:$PATH" "$CUP" "$@")
}

run_cup_expect_failure() (
    output_file=$1
    shift
    if run_cup "$@" >"$output_file" 2>&1; then
        fail "command unexpectedly succeeded: cup $*"
    fi
)

run_cup_expect_status() (
    output_file=$1
    expected_status=$2
    shift 2

    set +e
    run_cup "$@" >"$output_file" 2>&1
    status=$?
    set -e
    [ "$status" -eq "$expected_status" ] ||
        fail "cup $* returned status $status, expected $expected_status"
)

assert_cup_healthy() (
    assert_missing "$TEST_HOME/.cup/transaction.txt"
    run_cup doctor >/dev/null 2>&1
    if [ -d "$TEST_HOME/.cup/staging" ] &&
       find "$TEST_HOME/.cup/staging" -mindepth 1 -print -quit | grep . >/dev/null; then
        fail 'package runtime contains leftover staging content'
    fi
)


ensure_fixture_runtime_root() {
    root=$TEST_HOME/.cup
    mkdir -p "$root/components" "$root/staging" "$root/config" "$root/bin"
    chmod 0700 "$root"
    if [ ! -f "$root/root.txt" ]; then
        cat > "$root/root.txt" <<EOF_ROOT
format=2
product=coffee-clang/cup
layout=2
host=$TEST_PLATFORM
EOF_ROOT
    fi
    [ -f "$root/state.txt" ] || printf 'format=2\n' > "$root/state.txt"
    if [ ! -f "$root/cup.lock" ]; then
        : > "$root/cup.lock"
        chmod 0600 "$root/cup.lock"
    fi
    [ -f "$root/config/catalog.cfg" ] || cp "$DEV_ROOT/config/catalog.cfg" "$root/config/catalog.cfg"
}

package_catalog_next_index() {
    catalog=$DEV_ROOT/config/catalog.cfg
    awk -F'[.=]' '
        /^package\.[0-9]+\.component=/ {
            if (($2 + 0) >= n) n = ($2 + 0) + 1
        }
        END { print n + 0 }
    ' "$catalog"
}

package_catalog_add_package() {
    component=$1
    tool=$2
    target=$3
    version=$4
    format=$5
    url=$6
    sha=$7
    catalog=$DEV_ROOT/config/catalog.cfg
    temporary=$catalog.tmp
    index=$(package_catalog_next_index)

    # Integration fixtures add versions in semantic ascending order within a family.
    # Therefore the newly added record is the derived stable and prior records in
    # the same scope become non-stable.
    awk -v component="$component" -v tool="$tool" -v host="$TEST_PLATFORM" \
        -v target="$target" '
        function value(line) { sub(/^[^=]*=/, "", line); return line }
        /^package\.[0-9]+\.component=/ {
            split($0, a, "."); current = a[2]
            comp[current] = value($0)
        }
        /^package\.[0-9]+\.tool=/ { split($0, a, "."); t[a[2]] = value($0) }
        /^package\.[0-9]+\.host=/ { split($0, a, "."); h[a[2]] = value($0) }
        /^package\.[0-9]+\.target=/ { split($0, a, "."); tg[a[2]] = value($0) }
        /^package\.[0-9]+\.stable=/ {
            split($0, a, "."); i = a[2]
            if (comp[i] == component && t[i] == tool && h[i] == host && tg[i] == target)
                print "package." i ".stable=false"
            else
                print
            next
        }
        { print }
    ' "$catalog" > "$temporary" || fail 'could not update catalog stable records'
    mv "$temporary" "$catalog"

    revision=$(awk -F= '/^revision=/{print $2; exit}' "$catalog")
    case "$revision" in ''|*[!0-9]*) fail 'invalid fixture catalog revision' ;; esac
    next_revision=$((revision + 1))
    awk -v revision="$next_revision" '
        /^revision=/ { print "revision=" revision; next }
        { print }
    ' "$catalog" > "$temporary" || fail 'could not bump fixture catalog revision'
    mv "$temporary" "$catalog"

    {
        printf 'package.%s.component=%s\n' "$index" "$component"
        printf 'package.%s.tool=%s\n' "$index" "$tool"
        printf 'package.%s.host=%s\n' "$index" "$TEST_PLATFORM"
        printf 'package.%s.target=%s\n' "$index" "$target"
        printf 'package.%s.version=%s\n' "$index" "$version"
        case "$version" in
            *-rev*) printf 'package.%s.revision_reason=integration fixture revision\n' "$index" ;;
        esac
        printf 'package.%s.stable=true\n' "$index"
        printf 'package.%s.artifact.0.format=%s\n' "$index" "$format"
        printf 'package.%s.artifact.0.url=%s\n' "$index" "$url"
        printf 'package.%s.artifact.0.sha256=%s\n' "$index" "$sha"
    } >> "$catalog"

    # Keep the runtime snapshot in sync once a fixture runtime exists.
    if [ -f "$TEST_HOME/.cup/root.txt" ]; then
        cp "$catalog" "$TEST_HOME/.cup/config/catalog.cfg"
    fi
}

package_catalog_find_index() {
    tool=$1
    version=$2
    target=${3:-$TEST_PLATFORM}
    awk -v tool="$tool" -v version="$version" -v host="$TEST_PLATFORM" -v target="$target" '
        function value(line) { sub(/^[^=]*=/, "", line); return line }
        /^package\.[0-9]+\.tool=/ { split($0,a,"."); t[a[2]]=value($0) }
        /^package\.[0-9]+\.host=/ { split($0,a,"."); h[a[2]]=value($0) }
        /^package\.[0-9]+\.target=/ { split($0,a,"."); tg[a[2]]=value($0) }
        /^package\.[0-9]+\.version=/ {
            split($0,a,"."); i=a[2]; v=value($0)
            if (t[i] == tool && h[i] == host && tg[i] == target && v == version) { print i; exit }
        }
    ' "$DEV_ROOT/config/catalog.cfg"
}

package_catalog_rewrite_artifact() {
    tool=$1
    version=$2
    format=$3
    url=$4
    sha=$5
    target=${6:-$TEST_PLATFORM}
    catalog=$DEV_ROOT/config/catalog.cfg
    index=$(package_catalog_find_index "$tool" "$version" "$target")
    [ -n "$index" ] || fail "fixture catalog package not found: $tool@$version [$target]"
    temporary=$catalog.tmp
    awk -v prefix="package.$index.artifact.0." -v format="$format" -v url="$url" -v sha="$sha" '
        index($0, prefix "format=") == 1 { print prefix "format=" format; next }
        index($0, prefix "url=") == 1 { print prefix "url=" url; next }
        index($0, prefix "sha256=") == 1 { print prefix "sha256=" sha; next }
        { print }
    ' "$catalog" > "$temporary" || fail 'could not rewrite fixture artifact'
    mv "$temporary" "$catalog"
    [ ! -f "$TEST_HOME/.cup/root.txt" ] || cp "$catalog" "$TEST_HOME/.cup/config/catalog.cfg"
}

package_cache_publish() {
    archive=$1
    sha=$(hash_file "$archive")
    ensure_fixture_runtime_root
    mkdir -p "$TEST_HOME/.cup/cache"
    cp "$archive" "$TEST_HOME/.cup/cache/$sha"
    printf '%s\n' "$sha"
}

package_catalog_set_artifact() {
    tool=$1
    version=$2
    format=$3
    url=$4
    sha=$5
    catalog=$DEV_ROOT/config/catalog.cfg
    temporary=$catalog.tmp

    awk -v tool="$tool" -v version="$version" -v format="$format" \
        -v url="$url" -v sha="$sha" '
        function value(line) { sub(/^[^=]*=/, "", line); return line }
        /^package\.[0-9]+\.tool=/ { split($0,a,"."); tools[a[2]]=value($0) }
        /^package\.[0-9]+\.version=/ { split($0,a,"."); versions[a[2]]=value($0) }
        /^package\.[0-9]+\.artifact\.[0-9]+\.format=/ {
            split($0,a,"."); p=a[2]; art=a[4]
            if (tools[p] == tool && versions[p] == version && value($0) == format) selected[p "." art]=1
        }
        /^package\.[0-9]+\.artifact\.[0-9]+\.url=/ {
            split($0,a,"."); key=a[2] "." a[4]
            if (selected[key]) { print "package." a[2] ".artifact." a[4] ".url=" url; next }
        }
        /^package\.[0-9]+\.artifact\.[0-9]+\.sha256=/ {
            split($0,a,"."); key=a[2] "." a[4]
            if (selected[key]) { print "package." a[2] ".artifact." a[4] ".sha256=" sha; next }
        }
        { print }
    ' "$catalog" > "$temporary" || fail 'could not update fixture catalog artifact'
    mv "$temporary" "$catalog"
    [ ! -f "$TEST_HOME/.cup/root.txt" ] || cp "$catalog" "$TEST_HOME/.cup/config/catalog.cfg"
}


package_fixture_triple() {
    case "$1" in
        linux-x64) printf '%s\n' x86_64-linux-gnu ;;
        linux-arm64) printf '%s\n' aarch64-linux-gnu ;;
        windows-x64) printf '%s\n' x86_64-w64-mingw32 ;;
        macos-x64) printf '%s\n' x86_64-apple-darwin ;;
        macos-arm64) printf '%s\n' arm64-apple-darwin ;;
        *) fail "unsupported fixture platform: $1" ;;
    esac
}

package_fixture_family() {
    case "$1" in
        linux-*|windows-*) printf '%s\n' gnu ;;
        macos-*) printf '%s\n' darwin ;;
        *) fail "unsupported fixture platform: $1" ;;
    esac
}

package_fixture_runtime() {
    case "$1" in
        linux-*) printf '%s\n' glibc ;;
        windows-*) printf '%s\n' ucrt ;;
        macos-*) printf '%s\n' libSystem ;;
        *) fail "unsupported fixture platform: $1" ;;
    esac
}

package_fixture_source_name() {
    case "$1" in
        gcc) printf '%s\n' gcc ;;
        gdb) printf '%s\n' gdb ;;
        ld) printf '%s\n' binutils ;;
        clang|lld|lldb|clangd|clang-format|clang-tidy) printf '%s\n' llvm-project ;;
        valgrind) printf '%s\n' valgrind ;;
        *) fail "unsupported fixture tool: $1" ;;
    esac
}

package_fixture_source_version() {
    tool=$1
    version=$2
    printf '%s\n' "${version%%-rev*}"
}

write_package_revision_reason() {
    version=$1
    case "$version" in
        *-rev*) printf 'package.revision_reason=integration fixture revision\n' ;;
    esac
}

write_package_manifest() {
    package_root=$1
    path_list=$package_root/.manifest.paths

    (
        cd "$package_root"
        find . ! -path . ! -path './manifest.txt' ! -path './.manifest.paths' -print |
            sed 's#^\./##' | LC_ALL=C sort
    ) > "$path_list"
    {
        printf 'format=2\n'
        while IFS= read -r relative; do
            [ -n "$relative" ] || continue
            path=$package_root/$relative
            if [ -L "$path" ]; then
                target=$(readlink "$path") || fail "could not read fixture symlink: $relative"
                printf 'l\t-\t%s\t%s\n' "$(hash_text "$target")" "$relative"
            elif [ -d "$path" ]; then
                printf 'd\t0755\t-\t%s\n' "$relative"
            elif [ -f "$path" ]; then
                if [ -x "$path" ]; then mode=0755; else mode=0644; fi
                printf 'f\t%s\t%s\t%s\n' "$mode" "$(hash_file "$path")" "$relative"
            else
                fail "unsupported fixture package object: $relative"
            fi
        done < "$path_list"
    } > "$package_root/manifest.txt"
    rm -f "$path_list"
    chmod 0644 "$package_root/manifest.txt"
}

make_package() {
    component=$1
    tool=$2
    version=$3
    target=$4
    shift 4
    make_package_format "$component" "$tool" "$version" "$target" tar.gz "$@"
}

make_package_format() {
    component=$1
    tool=$2
    version=$3
    target=$4
    format=$5
    shift 5

    host=$TEST_PLATFORM
    package_name=$tool-$version-$host-$target
    package_root=$TMP_ROOT/packages/$package_name
    artifact_dir=$TMP_ROOT/artifacts
    archive=$artifact_dir/$package_name.$format

    rm -rf "$package_root"
    mkdir -p "$package_root/bin" "$artifact_dir"
    {
        printf 'package.component=%s\n' "$component"
        printf 'package.tool=%s\n' "$tool"
        printf 'package.version=%s\n' "$version"
        write_package_revision_reason "$version"
        printf 'platform.host=%s\n' "$host"
        printf 'platform.target=%s\n' "$target"
        printf 'platform.host_triple=%s\n' "$(package_fixture_triple "$host")"
        printf 'platform.target_triple=%s\n' "$(package_fixture_triple "$target")"
        printf 'platform.family=%s\n' "$(package_fixture_family "$target")"
        printf 'platform.runtime=%s\n' "$(package_fixture_runtime "$target")"
        printf 'platform.thread_model=posix\n'
        printf 'build.environment=test\n'
        printf 'build.source_policy=fixture\n'
        printf 'source.primary.name=%s\n' "$(package_fixture_source_name "$tool")"
        printf 'source.primary.version=%s\n' "$(package_fixture_source_version "$tool" "$version")"
        printf 'source.primary.url=https://example.invalid/%s-%s.tar.xz\n' \
            "$tool" "$(package_fixture_source_version "$tool" "$version")"
        printf 'source.primary.sha256=%064d\n' 0
        for entry in "$@"; do
            printf 'entry.%s=bin/%s\n' "$entry" "$entry"
        done
    } > "$package_root/info.txt"

    for entry in "$@"; do
        cat > "$package_root/bin/$entry" <<SCRIPT
#!/bin/sh
printf '%s\n' '$tool-$version-$target:$entry'
SCRIPT
        chmod +x "$package_root/bin/$entry"
    done
    write_package_manifest "$package_root"

    case "$format" in
        tar.gz) tar -czf "$archive" -C "$TMP_ROOT/packages" "$package_name" ;;
        tar.xz) tar -cJf "$archive" -C "$TMP_ROOT/packages" "$package_name" ;;
        zip)
            command -v zip >/dev/null 2>&1 || fail "zip utility is required for ZIP package fixtures"
            (cd "$TMP_ROOT/packages" && zip -qr "$archive" "$package_name")
            ;;
        *) fail "unsupported package fixture format: $format" ;;
    esac

    sha=$(hash_file "$archive")
    ensure_fixture_runtime_root
    mkdir -p "$TEST_HOME/.cup/cache"
    cp "$archive" "$TEST_HOME/.cup/cache/$sha"
    package_catalog_add_package "$component" "$tool" "$target" "$version" "$format" \
        "https://example.invalid/$package_name.$format" "$sha"
}

make_installed_package() {
    component=$1
    tool=$2
    version=$3
    target=$4
    shift 4

    root=$TEST_HOME/.cup/components/$component/$tool/$target/$version
    mkdir -p "$root/bin"
    {
        printf 'package.component=%s\n' "$component"
        printf 'package.tool=%s\n' "$tool"
        printf 'package.version=%s\n' "$version"
        write_package_revision_reason "$version"
        printf 'platform.host=%s\n' "$TEST_PLATFORM"
        printf 'platform.target=%s\n' "$target"
        printf 'platform.host_triple=%s\n' "$(package_fixture_triple "$TEST_PLATFORM")"
        printf 'platform.target_triple=%s\n' "$(package_fixture_triple "$target")"
        printf 'platform.family=%s\n' "$(package_fixture_family "$target")"
        printf 'platform.runtime=%s\n' "$(package_fixture_runtime "$target")"
        printf 'platform.thread_model=posix\n'
        printf 'build.environment=test\n'
        printf 'build.source_policy=fixture\n'
        printf 'source.primary.name=%s\n' "$(package_fixture_source_name "$tool")"
        printf 'source.primary.version=%s\n' "$(package_fixture_source_version "$tool" "$version")"
        printf 'source.primary.url=https://example.invalid/%s-%s.tar.xz\n' "$tool" "$(package_fixture_source_version "$tool" "$version")"
        printf 'source.primary.sha256=%064d\n' 0
        for entry in "$@"; do
            printf 'entry.%s=bin/%s\n' "$entry" "$entry"
        done
    } > "$root/info.txt"

    for entry in "$@"; do
        cat > "$root/bin/$entry" <<SCRIPT
#!/bin/sh
exit 0
SCRIPT
        chmod +x "$root/bin/$entry"
    done
    write_package_manifest "$root"
}

native_wrapper_path() {
    printf '%s/.cup/bin/%s\n' "$TEST_HOME" "$1"
}

run_native_wrapper() {
    entry=$1
    shift
    HOME="$TEST_HOME" "$(native_wrapper_path "$entry")" "$@"
}

require_test_binary() {
    assert_file "$TEST_BINARY"
    [ -x "$TEST_BINARY" ] || fail "test binary is not executable: $TEST_BINARY"
}

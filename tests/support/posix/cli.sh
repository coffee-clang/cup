
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
    cp "$PROJECT_ROOT/config/packages.cfg" "$DEV_ROOT/config/packages.cfg"
    cp "$PROJECT_ROOT/config/install.cfg" "$DEV_ROOT/config/install.cfg"

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
    cup_health_output=$(run_cup_with_managed_path doctor 2>&1)
    assert_contains "$cup_health_output" 'Doctor found no issues.'
    assert_not_contains "$cup_health_output" 'Error:'
    assert_not_contains "$cup_health_output" 'Issue:'
    assert_not_contains "$cup_health_output" 'Warning:'
    assert_not_contains "$cup_health_output" 'Incomplete:'
)


package_catalog_edit() {
    component=$1
    tool=$2
    target=$3
    field=$4
    value=$5
    mode=$6
    catalog=$DEV_ROOT/config/packages.cfg
    key="$component.$tool.$TEST_PLATFORM.$target.$field"
    temporary=$catalog.tmp

    awk -v key="$key" -v value="$value" -v mode="$mode" '
        BEGIN { found = 0 }
        index($0, key "=") == 1 {
            old = substr($0, length(key) + 2)
            if (mode == "prepend") {
                print key "=" value "," old
            } else if (mode == "replace") {
                print key "=" value
            } else {
                exit 3
            }
            found = 1
            next
        }
        { print }
        END { if (!found) exit 2 }
    ' "$catalog" >"$temporary" || {
        rm -f "$temporary"
        fail "catalog entry could not be updated: $key"
    }
    mv "$temporary" "$catalog"
}


package_catalog_ensure_package() {
    component=$1
    tool=$2
    target=$3
    version=$4
    format=${5:-tar.gz}
    catalog=$DEV_ROOT/config/packages.cfg
    key="$component.$tool.$TEST_PLATFORM.$target"

    grep -F "$key.stable_version=" "$catalog" >/dev/null 2>&1 && return 0

    package_path="$tool-{version}-{host_platform}-{target_platform}.{format}"
    url_template="https://example.invalid/$package_path"
    checksum_template="https://example.invalid/"
    checksum_path="$tool-{version}-{host_platform}-{target_platform}/SHA256SUMS"
    checksum_template="${checksum_template}${checksum_path}"
    cat >> "$catalog" <<EOF_PACKAGE_CATALOG

$key.stable_version=$version
$key.available_versions=$version
$key.default_format=$format
$key.formats=$format
$key.url_template=$url_template
$key.checksum_url_template=$checksum_template
EOF_PACKAGE_CATALOG
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
    if [ "$tool" = gcc ]; then
        printf '%s\n' "${version%%-rev*}"
    else
        printf '%s\n' "$version"
    fi
}

write_package_revision() {
    tool=$1
    version=$2

    [ "$tool" = gcc ] || return 0
    case "$version" in
        *-rev*) revision=${version##*-rev} ;;
        *) fail "invalid GCC fixture revision: $version" ;;
    esac
    case "$revision" in
        ''|0*|*[!0-9]*) fail "invalid GCC fixture revision: $version" ;;
    esac
    printf 'package.revision=%s\n' "$revision"
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
    cache_dir=$TEST_HOME/.cup/cache/$component/$tool/$host/$target/$version
    archive=$cache_dir/$package_name.$format

    rm -rf "$package_root"
    mkdir -p "$package_root/bin" "$cache_dir"
    {
        printf 'package.component=%s\n' "$component"
        printf 'package.tool=%s\n' "$tool"
        printf 'package.version=%s\n' "$version"
        write_package_revision "$tool" "$version"
        printf 'package.mode=self-contained\n'
        printf 'package.formats=tar.xz,tar.gz,zip\n'
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
        printf 'source.primary.url=https://example.invalid/%s-%s.tar.xz\n' "$tool" "$version"
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
        tar.gz)
            tar -czf "$archive" -C "$TMP_ROOT/packages" "$package_name"
            ;;
        tar.xz)
            tar -cJf "$archive" -C "$TMP_ROOT/packages" "$package_name"
            ;;
        zip)
            command -v zip >/dev/null 2>&1 ||
                fail "zip utility is required for ZIP package fixtures"
            (cd "$TMP_ROOT/packages" && zip -qr "$archive" "$package_name")
            ;;
        *)
            fail "unsupported package fixture format: $format"
            ;;
    esac
    {
        printf '%s  %s\n' "$(hash_file "$archive")" "$(basename "$archive")"
    } > "$cache_dir/SHA256SUMS"
}

make_installed_package() {
    component=$1
    tool=$2
    version=$3
    target=$4
    shift 4

    root=$TEST_HOME/.cup/components/$component/$tool/$TEST_PLATFORM/$target/$version
    mkdir -p "$root/bin"
    {
        printf 'package.component=%s\n' "$component"
        printf 'package.tool=%s\n' "$tool"
        printf 'package.version=%s\n' "$version"
        write_package_revision "$tool" "$version"
        printf 'package.mode=self-contained\n'
        printf 'package.formats=tar.xz,tar.gz,zip\n'
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
        printf 'source.primary.url=https://example.invalid/%s-%s.tar.xz\n' "$tool" "$version"
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

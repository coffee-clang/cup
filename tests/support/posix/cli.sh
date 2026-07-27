
# Purpose: Sourced POSIX integration library for isolated cup homes, catalogs,
# package fixtures and generated wrappers. This file is sourced, not executed.

: "${TESTS_ROOT:?TESTS_ROOT must be set before sourcing tests/support/posix/cli.sh}"
. "$TESTS_ROOT/support/common.sh"

. "$TESTS_ROOT/support/environment.sh"
cup_test_prepare_environment
TEST_PLATFORM=$CUP_TEST_PLATFORM
TEST_BINARY=${CUP_TEST_BINARY:-$PROJECT_ROOT/build/$TEST_PLATFORM/development/bin/cup}
export PROJECT_ROOT TEST_PLATFORM TEST_BINARY

prepare_command_environment() {
    TEST_HOME=$TMP_ROOT/home
    DEV_ROOT=$TMP_ROOT/development-root
    CUP=$TEST_BINARY
    export TEST_HOME DEV_ROOT CUP

    assert_file "$CUP"
    mkdir -p "$TEST_HOME" "$DEV_ROOT/config" "$DEV_ROOT/scripts/install"
    cp "$PROJECT_ROOT/config/packages.cfg" "$DEV_ROOT/config/packages.cfg"
    cp "$PROJECT_ROOT/config/install.cfg" "$DEV_ROOT/config/install.cfg"

    if [ "${TEST_PLATFORM%%-*}" = windows ]; then
        fail 'POSIX command environment cannot target Windows'
    fi

    cp "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" \
        "$DEV_ROOT/scripts/install/uninstall-cup.sh"
    chmod +x "$DEV_ROOT/scripts/install/uninstall-cup.sh"
}

run_cup() {
    (cd "$DEV_ROOT" && HOME="$TEST_HOME" "$CUP" "$@")
}

run_cup_expect_failure() (
    output_file=$1
    shift
    if run_cup "$@" >"$output_file" 2>&1; then
        fail "command unexpectedly succeeded: cup $*"
    fi
)

assert_cup_healthy() (
    cup_health_output=$(run_cup doctor 2>&1)
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
        printf 'platform.host=%s\n' "$host"
        printf 'platform.target=%s\n' "$target"
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

    case "$format" in
        tar.gz)
            tar -czf "$archive" -C "$TMP_ROOT/packages" "$package_name"
            ;;
        tar.xz)
            tar -cJf "$archive" -C "$TMP_ROOT/packages" "$package_name"
            ;;
        zip)
            need_zip=${CUP_TEST_ZIP_COMMAND:-zip}
            command -v "$need_zip" >/dev/null 2>&1 ||
                fail "zip utility is required for ZIP package fixtures"
            (cd "$TMP_ROOT/packages" && "$need_zip" -qr "$archive" "$package_name")
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
        printf 'platform.host=%s\n' "$TEST_PLATFORM"
        printf 'platform.target=%s\n' "$target"
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

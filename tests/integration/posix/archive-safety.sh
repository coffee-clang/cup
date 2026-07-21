#!/bin/sh

# Purpose: Exercises supported formats and malicious archive rejection through the real POSIX CLI.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin archive-safety
prepare_command_environment
run_cup repair >/dev/null
rm -f /tmp/cup-absolute-escape.txt

install_supported_format() {
    version=$1
    format=$2
    package_catalog_add_version compiler clang "$TEST_PLATFORM" "$version"
    package_catalog_set_format compiler clang "$TEST_PLATFORM" "$format"
    make_package_format compiler clang "$version" "$TEST_PLATFORM" "$format" clang
    run_cup install compiler "clang@$version" >/dev/null
    assert_contains "$(run_cup list compiler)" "compiler:clang@$version"
    assert_cup_healthy
}

install_supported_format 98.0.1 tar.gz
install_supported_format 98.0.2 tar.xz
install_supported_format 98.0.3 zip

# Invalid cached formats must trigger a bounded loopback refresh attempt rather
# than contacting the public release service during the integration suite.
awk '
    /compiler\.clang\..*\.url_template=/ {
        replacement = "=https://127.0.0.1:1/"
        replacement = replacement "{version}-{host_platform}-{target_platform}/"
        replacement = replacement "clang-{version}-{host_platform}-"
        replacement = replacement "{target_platform}.{format}"
        sub(/=.*/, replacement)
    }
    /compiler\.clang\..*\.checksum_url_template=/ {
        replacement = "=https://127.0.0.1:1/"
        replacement = replacement "{version}-{host_platform}-{target_platform}/"
        replacement = replacement "SHA256SUMS"
        sub(/=.*/, replacement)
    }
    { print }
' "$DEV_ROOT/config/packages.cfg" > "$DEV_ROOT/config/packages.cfg.tmp"
mv "$DEV_ROOT/config/packages.cfg.tmp" "$DEV_ROOT/config/packages.cfg"

create_mismatched_archive() {
    version=$1
    declared_format=$2
    actual_format=$3
    package_catalog_add_version compiler clang "$TEST_PLATFORM" "$version"
    package_catalog_set_format compiler clang "$TEST_PLATFORM" "$declared_format"
    make_package_format compiler clang "$version" "$TEST_PLATFORM" "$actual_format" clang

    package_name=clang-$version-$TEST_PLATFORM-$TEST_PLATFORM
    cache_dir=$TEST_HOME/.cup/cache/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/$version
    actual=$cache_dir/$package_name.$actual_format
    declared=$cache_dir/$package_name.$declared_format
    mv "$actual" "$declared"
    printf '%s  %s\n' "$(hash_file "$declared")" "$(basename "$declared")" \
        > "$cache_dir/SHA256SUMS"
}

create_plain_tar_disguised_as_gzip() {
    version=$1
    package_catalog_add_version compiler clang "$TEST_PLATFORM" "$version"
    package_catalog_set_format compiler clang "$TEST_PLATFORM" tar.gz

    package_name=clang-$version-$TEST_PLATFORM-$TEST_PLATFORM
    package_root=$TMP_ROOT/packages/$package_name
    cache_dir=$TEST_HOME/.cup/cache/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/$version
    archive=$cache_dir/$package_name.tar.gz
    rm -rf "$package_root"
    mkdir -p "$package_root/bin" "$cache_dir"
    cat > "$package_root/info.txt" <<EOF_INFO
package.component=compiler
package.tool=clang
package.version=$version
platform.host=$TEST_PLATFORM
platform.target=$TEST_PLATFORM
entry.clang=bin/clang
EOF_INFO
    printf '#!/bin/sh\nexit 0\n' > "$package_root/bin/clang"
    chmod +x "$package_root/bin/clang"
    tar -cf "$archive" -C "$TMP_ROOT/packages" "$package_name"
    printf '%s  %s\n' "$(hash_file "$archive")" "$(basename "$archive")" \
        > "$cache_dir/SHA256SUMS"
}

create_mismatched_archive 98.1.1 tar.xz tar.gz
run_cup_expect_failure "$TMP_ROOT/archive-format-mismatch.out" \
    install compiler clang@98.1.1
assert_contains "$(cat "$TMP_ROOT/archive-format-mismatch.out")" \
    "failed to download"
assert_not_contains "$(run_cup list compiler 2>/dev/null || true)" 'compiler:clang@98.1.1'

create_plain_tar_disguised_as_gzip 98.1.2
run_cup_expect_failure "$TMP_ROOT/archive-plain-tar.out" \
    install compiler clang@98.1.2
assert_contains "$(cat "$TMP_ROOT/archive-plain-tar.out")" \
    "failed to download"
assert_not_contains "$(run_cup list compiler 2>/dev/null || true)" 'compiler:clang@98.1.2'

package_catalog_set_format compiler clang "$TEST_PLATFORM" tar.gz

create_unsafe_archive() {
    version=$1
    mode=$2
    component=compiler
    tool=clang
    target=$TEST_PLATFORM
    host=$TEST_PLATFORM
    package_name=$tool-$version-$host-$target
    cache_dir=$TEST_HOME/.cup/cache/$component/$tool/$host/$target/$version
    archive=$cache_dir/$package_name.tar.gz
    mkdir -p "$cache_dir"

    fixture=$PROJECT_ROOT/build/$TEST_PLATFORM/${CUP_TEST_CONFIGURATION:-development}/tests/helpers/archive-fixture
    assert_file "$fixture"
    "$fixture" "$package_name" "$version" "$host" "$target" \
        "$archive" "$mode"
    printf '%s  %s\n' "$(hash_file "$archive")" "$(basename "$archive")" \
        > "$cache_dir/SHA256SUMS"
}

index=1
for case in traversal absolute symlink symlink-parent duplicate case-collision \
        file-directory reserved unicode special hardlink-forward root-file; do
    version="99.0.$index"
    index=$((index + 1))
    package_catalog_add_version compiler clang "$TEST_PLATFORM" "$version"
    create_unsafe_archive "$version" "$case"
    run_cup_expect_failure "$TMP_ROOT/archive-$case.out" \
        install compiler "clang@$version"
    assert_not_contains "$(run_cup list compiler 2>/dev/null || true)" "compiler:clang@$version"
    assert_missing "$TMP_ROOT/outside.txt"
    assert_missing /tmp/cup-absolute-escape.txt
    assert_missing "$TEST_HOME/.cup/transaction.txt"
done

assert_cup_healthy
printf 'Archive format and safety integration tests passed for %s.\n' "$TEST_PLATFORM"

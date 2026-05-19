#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<USAGE
Usage:
  $0 <llvm-tool>

Examples:
  $0 clang
  $0 clangd
USAGE
}

if [ "$#" -ne 1 ]; then
    usage >&2
    exit 2
fi

LLVM_TOOL="$1"
source dist/release.env

rm -rf dist/package-test
mkdir -p dist/package-test
tar -xJf "dist/$package_base.tar.xz" -C dist/package-test

root="dist/package-test/$package_base"

require_executable() {
    local path="$1"

    if [ ! -x "$path" ]; then
        echo "missing executable: $path" >&2
        exit 1
    fi
}

case "$LLVM_TOOL" in
    clang)
        require_executable "$root/bin/clang"
        require_executable "$root/bin/clang++"
        require_executable "$root/bin/ld.lld"

        "$root/bin/clang" --version
        "$root/bin/clang++" --version
        "$root/bin/ld.lld" --version

        resource_dir="$($root/bin/clang -print-resource-dir)"
        echo "clang resource dir: $resource_dir"
        test -d "$resource_dir"

        cat > /tmp/cup-clang-test.c <<'C_EOF'
#include <stdio.h>

static int add(int a, int b) {
    return a + b;
}

int main(void) {
    printf("hello clang %d\n", add(20, 22));
    return 0;
}
C_EOF
        "$root/bin/clang" /tmp/cup-clang-test.c -o /tmp/cup-clang-test
        /tmp/cup-clang-test | grep -F "hello clang 42"

        "$root/bin/clang" -fuse-ld=lld /tmp/cup-clang-test.c -o /tmp/cup-clang-lld-test
        /tmp/cup-clang-lld-test | grep -F "hello clang 42"

        "$root/bin/clang" -flto -fuse-ld=lld /tmp/cup-clang-test.c -o /tmp/cup-clang-lto-test
        /tmp/cup-clang-lto-test | grep -F "hello clang 42"

        cat > /tmp/cup-clang-cpp-test.cpp <<'CPP_EOF'
#include <iostream>
#include <vector>

int main() {
    std::vector<int> values = {20, 22};
    std::cout << (values[0] + values[1]) << "\n";
    return 0;
}
CPP_EOF
        "$root/bin/clang++" /tmp/cup-clang-cpp-test.cpp -o /tmp/cup-clang-cpp-test
        /tmp/cup-clang-cpp-test | grep -F "42"
        ;;
    lld)
        require_executable "$root/bin/lld"
        require_executable "$root/bin/ld.lld"

        "$root/bin/lld" --version
        "$root/bin/ld.lld" --version

        cat > /tmp/cup-lld-test.c <<'C_EOF'
#include <stdio.h>

int main(void) {
    printf("hello lld\n");
    return 0;
}
C_EOF
        cc -B"$root/bin" -fuse-ld=lld /tmp/cup-lld-test.c -o /tmp/cup-lld-test
        /tmp/cup-lld-test | grep -F "hello lld"
        ;;
    lldb)
        require_executable "$root/bin/lldb"

        "$root/bin/lldb" --version
        "$root/bin/lldb" -b -o "script import sys; print('python-ok', sys.version_info[0], sys.version_info[1])" -o quit

        cat > /tmp/cup-lldb-test.c <<'C_EOF'
#include <stdio.h>

static int add(int a, int b) {
    return a + b;
}

int main(void) {
    int x = add(20, 22);
    printf("x = %d\n", x);
    return 0;
}
C_EOF
        cc -g -O0 /tmp/cup-lldb-test.c -o /tmp/cup-lldb-test
        "$root/bin/lldb" -b \
            -o "target create /tmp/cup-lldb-test" \
            -o "breakpoint set --name add" \
            -o "run" \
            -o "frame variable a" \
            -o "frame variable b" \
            -o "bt" \
            -o "quit" | tee /tmp/cup-lldb-output.txt
        grep -F "(int) a = 20" /tmp/cup-lldb-output.txt
        grep -F "(int) b = 22" /tmp/cup-lldb-output.txt
        ;;
    clangd)
        require_executable "$root/bin/clangd"

        "$root/bin/clangd" --version
        tmpdir="/tmp/cup-clangd-project"
        rm -rf "$tmpdir"
        mkdir -p "$tmpdir"
        cat > "$tmpdir/main.c" <<'C_EOF'
#include <stdio.h>

int main(void) {
    printf("hello clangd\n");
    return 0;
}
C_EOF
        cat > "$tmpdir/compile_commands.json" <<EOF_JSON
[
  {
    "directory": "$tmpdir",
    "command": "cc -std=c11 -I$tmpdir main.c",
    "file": "$tmpdir/main.c"
  }
]
EOF_JSON
        "$root/bin/clangd" --check="$tmpdir/main.c" | tee /tmp/cup-clangd-output.txt
        grep -E "All checks completed|Testing on source file" /tmp/cup-clangd-output.txt
        ;;
    clang-format)
        require_executable "$root/bin/clang-format"

        "$root/bin/clang-format" --version
        printf "%s\n" "int main( void ){return 0;}" > /tmp/cup-format-test.c
        "$root/bin/clang-format" /tmp/cup-format-test.c | tee /tmp/cup-format-output.c
        grep -F "int main(void)" /tmp/cup-format-output.c
        ;;
    clang-tidy)
        require_executable "$root/bin/clang-tidy"

        "$root/bin/clang-tidy" --version
        "$root/bin/clang-tidy" --list-checks -checks=clang-analyzer-* | grep -F "clang-analyzer-core"
        cat > /tmp/cup-tidy-test.c <<'C_EOF'
int main(void) {
    return 0;
}
C_EOF
        "$root/bin/clang-tidy" /tmp/cup-tidy-test.c -- -std=c11
        ;;
    *)
        echo "unsupported LLVM tool: $LLVM_TOOL" >&2
        exit 2
        ;;
esac

#!/usr/bin/env bash
set -u

TEST_HOME="$(mktemp -d)"
trap 'rm -rf "${TEST_HOME}"' EXIT

run() {
    echo
    echo "============================================================"
    echo "$ $*"
    echo "------------------------------------------------------------"
    HOME="${TEST_HOME}" "$@"
    status=$?
    echo "------------------------------------------------------------"
    echo "exit code: ${status}"
}

CUP="./cup"

echo "Running cup error-output checks"

# Command parsing
run "${CUP}"
run "${CUP}" unknown

# list
run "${CUP}" list
run "${CUP}" list extra
run "${CUP}" list --foo
run "${CUP}" list --platform
run "${CUP}" list --platform linux-x64
run "${CUP}" list --platform windows-x64
run "${CUP}" list --platform linux-x64 --platform windows-x64
run "${CUP}" list --format tar.xz

# install missing args
run "${CUP}" install
run "${CUP}" install compiler
run "${CUP}" install compiler gcc@stable

# install invalid options
run "${CUP}" install debugger gdb@stable --foo
run "${CUP}" install debugger gdb@stable --foo value
run "${CUP}" install debugger gdb@stable --format
run "${CUP}" install debugger gdb@stable --format tar.xz --format tar.gz
run "${CUP}" install debugger gdb@stable -f tar.xz --format tar.gz
run "${CUP}" install debugger gdb@stable --platform
run "${CUP}" install debugger gdb@stable --platform linux-x64 --platform windows-x64
run "${CUP}" install debugger gdb@stable --platform linux-x64 --format tar.xz
run "${CUP}" install debugger gdb@stable --format tar.xz --platform linux-x64

# invalid platform
run "${CUP}" install debugger gdb@stable --platform fake-x64
run "${CUP}" install debugger gdb@stable --platform linux-fake
run "${CUP}" install debugger gdb@stable --platform windows-x64

# invalid component/tool/entry
run "${CUP}" install unknown gdb@stable
run "${CUP}" install debugger unknown@stable
run "${CUP}" install debugger gdb
run "${CUP}" install debugger @stable
run "${CUP}" install debugger gdb@
run "${CUP}" install debugger gdb@stable@extra

# invalid version/format
run "${CUP}" install debugger gdb@999.999
run "${CUP}" install debugger gdb@stable --format zip

# remove
run "${CUP}" remove
run "${CUP}" remove debugger
run "${CUP}" remove debugger gdb@stable
run "${CUP}" remove debugger gdb@stable --format tar.xz
run "${CUP}" remove debugger gdb@stable --platform
run "${CUP}" remove debugger gdb@stable --platform linux-x64

# default
run "${CUP}" default
run "${CUP}" default debugger
run "${CUP}" default debugger gdb@stable
run "${CUP}" default debugger gdb@stable --format tar.xz
run "${CUP}" default debugger gdb@stable --platform
run "${CUP}" default debugger gdb@stable --platform linux-x64

# current
run "${CUP}" current
run "${CUP}" current debugger
run "${CUP}" current debugger --format tar.xz
run "${CUP}" current debugger --platform
run "${CUP}" current debugger --platform linux-x64
run "${CUP}" current debugger --platform windows-x64
#!/usr/bin/env sh

# Purpose: Separates native symbols and strips the public release executable.
set -eu

platform=${1:?platform is required}
binary=${2:?binary is required}
output=${3:?symbol output directory is required}
mkdir -p "$output"

case "$platform" in
    linux-*)
        command -v objcopy >/dev/null 2>&1 && command -v strip >/dev/null 2>&1 || {
            echo "Error: objcopy and strip are required for Linux release finalization." >&2
            exit 2
        }
        objcopy --only-keep-debug "$binary" "$output/cup.debug"
        strip --strip-unneeded "$binary"
        objcopy --add-gnu-debuglink="$output/cup.debug" "$binary"
        ;;
    macos-*)
        command -v dsymutil >/dev/null 2>&1 && command -v strip >/dev/null 2>&1 || {
            echo "Error: dsymutil and strip are required for macOS release finalization." >&2
            exit 2
        }
        dsymutil "$binary" -o "$output/cup.dSYM"
        strip -S "$binary"
        ;;
    windows-x64)
        command -v objcopy >/dev/null 2>&1 && command -v strip >/dev/null 2>&1 || {
            echo "Error: objcopy and strip are required for Windows release finalization." >&2
            exit 2
        }
        objcopy --only-keep-debug "$binary" "$output/cup.debug"
        strip --strip-unneeded "$binary"
        # Keep symbols as a separate release artifact. GNU debug links are not
        # required by the Windows packaging or native test flow.
        ;;
    *)
        echo "Error: unsupported release platform: $platform" >&2
        exit 2
        ;;
esac

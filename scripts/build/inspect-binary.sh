#!/bin/sh

# Purpose: Verifies the native executable format, architecture and dynamic-link
# policy for one CUP build and writes a deterministic inspection report.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ

platform=${1:?platform is required}
configuration=${2:?configuration is required}
binary=${3:?binary path is required}
report=${4:?report path is required}

fail() {
    printf 'binary inspection: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found"
}

hash_binary() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$binary" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$binary" | awk '{print $1}'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

write_report() {
    temporary=$report.tmp.$$
    mkdir -p "$(dirname "$report")"
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    cat >"$temporary"
    if [ -f "$report" ] && cmp -s "$temporary" "$report"; then
        rm -f "$temporary"
    else
        mv -f "$temporary" "$report"
    fi
    trap - EXIT HUP INT TERM
}

case "$configuration" in
    development|debug|coverage|sanitizers|release) ;;
    *) fail "unsupported configuration '$configuration'" ;;
esac

[ -f "$binary" ] || fail "binary does not exist: $binary"
[ -s "$binary" ] || fail "binary is empty: $binary"
require_tool file

sha256=$(hash_binary)
file_description=$(file -b "$binary") || fail "file could not inspect $binary"

# ELF policy: architecture, dynamic loader, dependencies and search paths.
inspect_elf() {
    case "$platform" in
        linux-x64)
            expected_machine='Advanced Micro Devices X86-64'
            architecture=x86_64
            ;;
        linux-arm64)
            expected_machine=AArch64
            architecture=aarch64
            ;;
        *) fail "platform '$platform' is not supported by the ELF inspector" ;;
    esac

    case "$file_description" in
        *ELF*) ;;
        *) fail "expected an ELF executable, got: $file_description" ;;
    esac
    require_tool readelf

    elf_header=$(readelf -h "$binary") || fail "readelf could not read the ELF header"
    header_value() {
        key=$1
        printf '%s\n' "$elf_header" | awk -F: -v key="$key" '
            $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
                value=$0
                sub(/^[^:]*:[[:space:]]*/, "", value)
                print value
                exit
            }
        '
    }

    elf_class=$(header_value Class)
    elf_data=$(header_value Data)
    elf_type=$(header_value Type)
    machine=$(header_value Machine)
    [ "$elf_class" = ELF64 ] || fail "expected ELF64, got '$elf_class'"
    [ "$machine" = "$expected_machine" ] ||
        fail "machine '$machine' does not match $platform"

    program_headers=$(readelf -l "$binary") || fail "readelf could not read program headers"
    interpreter=$(printf '%s\n' "$program_headers" | awk '
        /Requesting program interpreter:/ {
            value=$0
            sub(/^.*Requesting program interpreter:[[:space:]]*/, "", value)
            sub(/\]$/, "", value)
            print value
            exit
        }
    ')

    dynamic_output=$(readelf -d "$binary" 2>&1 || true)
    needed=$(printf '%s\n' "$dynamic_output" | awk '
        /\(NEEDED\)/ {
            value=$0
            sub(/^.*\[/, "", value)
            sub(/\].*$/, "", value)
            print value
        }
    ' | LC_ALL=C sort -u)
    search_paths=$(printf '%s\n' "$dynamic_output" | awk '
        /\((RPATH|RUNPATH)\)/ {
            value=$0
            sub(/^.*\[/, "", value)
            sub(/\].*$/, "", value)
            print value
        }
    ' | LC_ALL=C sort -u)

    [ -z "$search_paths" ] ||
        fail "RPATH/RUNPATH is not allowed: $(printf '%s' "$search_paths" | tr '\n' ' ')"

    for library in $needed; do
        case "$library" in
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libacl.so.*) ;;
            libgcc_s.so.*|libasan.so.*|libubsan.so.*|libstdc++.so.*|libclang_rt.*.so*) ;;
            *) fail "library is outside the Linux system/runtime allowlist: $library" ;;
        esac
    done

    if [ "$configuration" = release ]; then
        [ -z "$needed" ] || fail 'release executable has dynamic dependencies'
        [ -z "$interpreter" ] ||
            fail "release executable has a dynamic interpreter: $interpreter"
        linkage=static
    else
        [ -n "$needed" ] || fail "$configuration executable has no dynamic system dependencies"
        [ -n "$interpreter" ] || fail "$configuration executable has no dynamic interpreter"
        linkage=dynamic-system
    fi

    needed_count=$(printf '%s\n' "$needed" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=1\n'
        printf 'platform=%s\n' "$platform"
        printf 'configuration=%s\n' "$configuration"
        printf 'binary=%s\n' "$(basename "$binary")"
        printf 'sha256=%s\n' "$sha256"
        printf 'object_format=ELF\n'
        printf 'architecture=%s\n' "$architecture"
        printf 'elf_class=%s\n' "$elf_class"
        printf 'elf_data=%s\n' "$elf_data"
        printf 'elf_type=%s\n' "$elf_type"
        printf 'machine=%s\n' "$machine"
        printf 'linkage=%s\n' "$linkage"
        printf 'interpreter=%s\n' "${interpreter:-none}"
        printf 'needed_count=%s\n' "$needed_count"
        if [ -n "$needed" ]; then
            printf '%s\n' "$needed" | while IFS= read -r library; do
                [ -n "$library" ] && printf 'needed=%s\n' "$library"
            done
        fi
        printf 'runtime_search_path=none\n'
        printf 'file_description=%s\n' "$file_description"
    } | write_report
}

# Mach-O policy: architecture, deployment target and Apple-only dependencies.
inspect_macho() {
    case "$platform" in
        macos-x64)
            expected_arch=x86_64
            architecture=x86_64
            ;;
        macos-arm64)
            expected_arch=arm64
            architecture=arm64
            ;;
        *) fail "platform '$platform' is not supported by the Mach-O inspector" ;;
    esac

    case "$file_description" in
        *Mach-O*) ;;
        *) fail "expected a Mach-O executable, got: $file_description" ;;
    esac
    require_tool lipo
    require_tool otool

    archs=$(lipo -archs "$binary") || fail "lipo could not inspect $binary"
    [ "$archs" = "$expected_arch" ] ||
        fail "architectures '$archs' do not match $platform"

    libraries=$(otool -L "$binary" | awk 'NR > 1 { print $1 }' | LC_ALL=C sort -u)
    [ -n "$libraries" ] || fail 'Mach-O executable has no dynamic system dependencies'
    for library in $libraries; do
        case "$library" in
            /usr/lib/*|/System/Library/Frameworks/*) ;;
            *) fail "library is outside the macOS system/framework allowlist: $library" ;;
        esac
    done

    load_commands=$(otool -l "$binary") || fail "otool could not read load commands"
    rpaths=$(printf '%s\n' "$load_commands" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rpath=1; next }
        in_rpath && $1 == "path" { print $2; in_rpath=0 }
    ' | LC_ALL=C sort -u)
    [ -z "$rpaths" ] ||
        fail "LC_RPATH is not allowed: $(printf '%s' "$rpaths" | tr '\n' ' ')"

    minimum_os=$(printf '%s\n' "$load_commands" | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" { mode="build"; next }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" { mode="legacy"; next }
        mode == "build" && $1 == "minos" { print $2; exit }
        mode == "legacy" && $1 == "version" { print $2; exit }
    ')
    [ -n "$minimum_os" ] || fail 'Mach-O executable does not declare a minimum macOS version'
    case "$minimum_os" in
        13.0|13.0.0) minimum_os=13.0 ;;
        *) fail "Mach-O minimum macOS version '$minimum_os' does not match policy 13.0" ;;
    esac

    needed_count=$(printf '%s\n' "$libraries" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=1\n'
        printf 'platform=%s\n' "$platform"
        printf 'configuration=%s\n' "$configuration"
        printf 'binary=%s\n' "$(basename "$binary")"
        printf 'sha256=%s\n' "$sha256"
        printf 'object_format=Mach-O\n'
        printf 'architecture=%s\n' "$architecture"
        printf 'minimum_os=%s\n' "$minimum_os"
        printf 'linkage=dynamic-system\n'
        printf 'needed_count=%s\n' "$needed_count"
        printf '%s\n' "$libraries" | while IFS= read -r library; do
            [ -n "$library" ] && printf 'needed=%s\n' "$library"
        done
        printf 'runtime_search_path=none\n'
        printf 'file_description=%s\n' "$file_description"
    } | write_report
}

# PE policy helpers and the Windows system-DLL allowlist.
find_pe_objdump() {
    if [ -n "${CUP_OBJDUMP:-}" ]; then
        command -v "$CUP_OBJDUMP" >/dev/null 2>&1 ||
            fail "configured objdump '$CUP_OBJDUMP' was not found"
        printf '%s\n' "$CUP_OBJDUMP"
        return
    fi
    for candidate in x86_64-w64-mingw32-objdump llvm-objdump objdump; do
        if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    fail 'no PE-capable objdump was found'
}

inspect_pe() {
    [ "$platform" = windows-x64 ] ||
        fail "platform '$platform' is not supported by the PE inspector"
    case "$file_description" in
        *PE32+*x86-64*|*PE32+*x86_64*) ;;
        *) fail "expected a PE32+ x86-64 executable, got: $file_description" ;;
    esac

    objdump=$(find_pe_objdump)
    coff_header=$($objdump -f "$binary") || fail "$objdump could not read the COFF header"
    printf '%s\n' "$coff_header" | grep -Eiq 'architecture:[[:space:]]*(i386:x86-64|x86-64|amd64)' ||
        fail 'PE/COFF architecture does not match windows-x64'

    pe_headers=$($objdump -p "$binary") || fail "$objdump could not read PE headers"
    subsystem=$(printf '%s\n' "$pe_headers" | awk '
        $1 == "Subsystem" {
            value=$0
            sub(/^[[:space:]]*Subsystem[[:space:]]+[^[:space:]]+[[:space:]]*/, "", value)
            gsub(/[()]/, "", value)
            print value
            exit
        }
    ')
    [ "$subsystem" = 'Windows CUI' ] || fail "unexpected PE subsystem: ${subsystem:-missing}"

    imports=$(printf '%s\n' "$pe_headers" | awk '
        $1 == "DLL" && $2 == "Name:" { print $3 }
    ' | LC_ALL=C sort -fu)
    [ -n "$imports" ] || fail 'PE executable has no imported system DLLs'
    for library in $imports; do
        normalized=$(printf '%s' "$library" | tr '[:upper:]' '[:lower:]')
        case "$normalized" in
            advapi32.dll|bcrypt.dll|comctl32.dll|crypt32.dll|dnsapi.dll|gdi32.dll) ;;
            iphlpapi.dll|kernel32.dll|msvcrt.dll|ntdll.dll|ole32.dll|oleaut32.dll) ;;
            secur32.dll|shell32.dll|shlwapi.dll|user32.dll|version.dll|winhttp.dll) ;;
            winmm.dll|ws2_32.dll|ucrtbase.dll|api-ms-win-*.dll|ext-ms-win-*.dll) ;;
            *) fail "library is outside the Windows system DLL allowlist: $library" ;;
        esac
    done

    resource_line=$(printf '%s\n' "$pe_headers" | awk '/Resource Directory/ { print; exit }')
    [ -n "$resource_line" ] || fail 'PE executable has no resource directory'
    resource_size=$(printf '%s\n' "$resource_line" | awk '{ print $4 }')
    case "$resource_size" in
        ''|0|00000000|0000000000000000) fail 'PE resource directory is empty' ;;
    esac

    for hardening in DYNAMIC_BASE NX_COMPAT; do
        printf '%s\n' "$pe_headers" | grep -Eq "^[[:space:]]*$hardening([[:space:]]|$)" ||
            fail "PE executable is missing $hardening"
    done

    needed_count=$(printf '%s\n' "$imports" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=1\n'
        printf 'platform=%s\n' "$platform"
        printf 'configuration=%s\n' "$configuration"
        printf 'binary=%s\n' "$(basename "$binary")"
        printf 'sha256=%s\n' "$sha256"
        printf 'object_format=PE32+\n'
        printf 'architecture=x86_64\n'
        printf 'subsystem=%s\n' "$subsystem"
        printf 'linkage=dynamic-system\n'
        printf 'needed_count=%s\n' "$needed_count"
        printf '%s\n' "$imports" | while IFS= read -r library; do
            [ -n "$library" ] && printf 'needed=%s\n' "$library"
        done
        printf 'resource_directory=present\n'
        printf 'dynamic_base=yes\n'
        printf 'nx_compat=yes\n'
        printf 'runtime_search_path=none\n'
        printf 'file_description=%s\n' "$file_description"
    } | write_report
}

case "$platform" in
    linux-x64|linux-arm64) inspect_elf ;;
    macos-x64|macos-arm64) inspect_macho ;;
    windows-x64) inspect_pe ;;
    *) fail "unsupported platform '$platform'" ;;
esac

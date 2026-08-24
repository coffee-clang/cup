#!/bin/sh

# Verifies one native executable against the selected build
# configuration and writes a stable machine-readable inspection report.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# shellcheck source=../lib/path-safety.sh
. "$SCRIPT_DIR/../lib/path-safety.sh"

platform=${1:?platform is required}
configuration=${2:?configuration is required}
binary=${3:?binary path is required}
report=${4:?report path is required}
inspection_policy=${5:-build}

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
    if [ -n "${CUP_BUILD_ROOT:-}" ]; then
        cup_path_require_build_root "$CUP_BUILD_ROOT" || fail "invalid build root: $CUP_BUILD_ROOT"
        cup_path_prepare_child_file "$CUP_BUILD_ROOT" "$report" "inspection report" || exit 1
    else
        cup_path_prepare_file_target "$report" "inspection report" || exit 1
    fi
    cup_path_write_file "$report" 0644 if-different ||
        fail "could not write inspection report: $report"
}

case "$configuration" in
    development|debug|coverage|sanitizers|release) ;;
    *) fail "unsupported configuration '$configuration'" ;;
esac
case "$inspection_policy" in build|public) ;; *) fail "unsupported inspection policy '$inspection_policy'" ;; esac
[ "$inspection_policy" = build ] || [ "$configuration" = release ] ||
    fail 'public inspection is valid only for release candidates'
cup_path_require_regular_file "$binary" "binary" ||
    fail "binary is not a safe regular file: $binary"
[ -s "$binary" ] || fail "binary is empty: $binary"
[ -x "$binary" ] || fail "binary is not executable: $binary"
require_tool file
sha256=$(hash_binary)
file_description=$(file -b "$binary") || fail "file could not inspect $binary"

has_debug_sections_elf() {
    printf '%s\n' "$1" | grep -E '\.(debug_info|zdebug_info)' >/dev/null
}

write_needed_entries() {
    while IFS= read -r library; do
        [ -z "$library" ] || printf 'needed=%s\n' "$library"
    done
}

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
        *)
            fail "platform '$platform' is not supported by the ELF inspector"
            ;;
    esac
    case "$file_description" in
        *ELF*executable*|*ELF*pie*executable*)
            ;;
        *)
            fail "expected an ELF executable, got: $file_description"
            ;;
    esac
    require_tool readelf

    elf_header=$(readelf -h "$binary") || fail 'readelf could not read the ELF header'
    header_value() {
        key=$1
        printf '%s\n' "$elf_header" | awk -F: -v key="$key" '
            $1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
                sub(/^[^:]*:[[:space:]]*/, "")
                print
                exit
            }
        '
    }
    elf_class=$(header_value Class)
    elf_data=$(header_value Data)
    elf_type=$(header_value Type)
    machine=$(header_value Machine)
    entry_point=$(header_value 'Entry point address')
    [ "$elf_class" = ELF64 ] || fail "expected ELF64, got '$elf_class'"
    case "$elf_data" in
        *little*endian*)
            ;;
        *)
            fail "expected little-endian ELF, got '$elf_data'"
            ;;
    esac
    [ "$machine" = "$expected_machine" ] || fail "machine '$machine' does not match $platform"
    case "$elf_type" in
        EXEC*|DYN*)
            ;;
        *)
            fail "ELF object is not an executable: $elf_type"
            ;;
    esac
    case "$entry_point" in
        ''|0x0|0)
            fail 'ELF executable has no entry point'
            ;;
    esac

    program_headers=$(readelf -l "$binary") || fail 'readelf could not read program headers'
    printf '%s\n' "$program_headers" | grep -E '^[[:space:]]*LOAD[[:space:]]' >/dev/null ||
        fail 'ELF executable has no LOAD segment'
    interpreter=$(printf '%s\n' "$program_headers" | awk '
        /Requesting program interpreter:/ {
            sub(/^.*Requesting program interpreter:[[:space:]]*/, "")
            sub(/\]$/, "")
            print
            exit
        }
    ')
    dynamic_output=$(readelf -d "$binary" 2>&1 || true)
    needed=$(printf '%s\n' "$dynamic_output" |
        awk '/\(NEEDED\)/ { sub(/^.*\[/, ""); sub(/\].*$/, ""); print }' |
        sort -u)
    search_paths=$(printf '%s\n' "$dynamic_output" |
        awk '/\((RPATH|RUNPATH)\)/ { sub(/^.*\[/, ""); sub(/\].*$/, ""); print }' |
        sort -u)
    [ -z "$search_paths" ] || fail "RPATH/RUNPATH is not allowed: $(printf '%s' "$search_paths" | tr '\n' ' ')"

    for library in $needed; do
        case "$library" in
            libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
                libresolv.so.*|libacl.so.*|ld-linux*.so.*|libgcc_s.so.*) ;;
            libasan.so.*|libubsan.so.*|libclang_rt.*.so*)
                [ "$configuration" = sanitizers ] ||
                    fail "sanitizer runtime is not allowed for $configuration: $library"
                ;;
            *) fail "library is outside the Linux system/runtime allowlist: $library" ;;
        esac
    done

    sections=$(readelf -S "$binary") || fail 'readelf could not read ELF sections'
    symbols=$(readelf -Ws "$binary" 2>/dev/null || true)
    case "$configuration" in
        debug)
            has_debug_sections_elf "$sections" ||
                fail 'debug executable contains no DWARF information'
            ;;
        coverage)
            printf '%s\n' "$symbols" | grep -E '__gcov_(init|exit|merge)' >/dev/null ||
                fail 'coverage executable contains no gcov instrumentation'
            ;;
        sanitizers)
            printf '%s\n%s\n' "$symbols" "$needed" |
                grep -E '__asan_init|libasan|libclang_rt.*asan' >/dev/null ||
                fail 'sanitizer executable contains no ASan runtime'
            printf '%s\n%s\n' "$symbols" "$needed" |
                grep -E '__ubsan_handle_|libubsan|libclang_rt.*ubsan' >/dev/null ||
                fail 'sanitizer executable contains no UBSan runtime'
            ;;
    esac

    if [ "$configuration" = release ]; then
        [ -z "$needed" ] || fail 'release executable has dynamic dependencies'
        [ -z "$interpreter" ] || fail "release executable has a dynamic interpreter: $interpreter"
        linkage=static
        if [ "$inspection_policy" = public ]; then
            ! has_debug_sections_elf "$sections" || fail 'public release executable still contains DWARF information'
        fi
    else
        [ -n "$needed" ] || fail "$configuration executable has no dynamic system dependencies"
        [ -n "$interpreter" ] || fail "$configuration executable has no dynamic interpreter"
        linkage=dynamic-system
    fi

    needed_count=$(printf '%s\n' "$needed" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=2\nplatform=%s\nconfiguration=%s\ninspection_policy=%s\n' \
            "$platform" "$configuration" "$inspection_policy"
        printf 'binary=%s\nsha256=%s\nobject_format=ELF\narchitecture=%s\n' \
            "$(basename "$binary")" "$sha256" "$architecture"
        printf 'elf_class=%s\nelf_data=%s\nelf_type=%s\nmachine=%s\nentry_point=%s\n' \
            "$elf_class" "$elf_data" "$elf_type" "$machine" "$entry_point"
        printf 'linkage=%s\ninterpreter=%s\nneeded_count=%s\n' "$linkage" "${interpreter:-none}" "$needed_count"
        [ -z "$needed" ] || printf '%s\n' "$needed" | write_needed_entries
        printf 'runtime_search_path=none\nfile_description=%s\n' "$file_description"
    } | write_report
}

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
        *)
            fail "platform '$platform' is not supported by the Mach-O inspector"
            ;;
    esac
    case "$file_description" in
        *Mach-O*executable*)
            ;;
        *)
            fail "expected a Mach-O executable, got: $file_description"
            ;;
    esac
    require_tool lipo
    require_tool otool
    require_tool nm
    archs=$(lipo -archs "$binary") || fail "lipo could not inspect $binary"
    [ "$archs" = "$expected_arch" ] || fail "architectures '$archs' do not match $platform"
    header=$(otool -hv "$binary") || fail 'otool could not read Mach-O header'
    printf '%s\n' "$header" | grep -E 'EXECUTE|MH_EXECUTE' >/dev/null || fail 'Mach-O object is not an executable'

    libraries=$(otool -L "$binary" | awk 'NR > 1 { print $1 }' | sort -u)
    [ -n "$libraries" ] || fail 'Mach-O executable has no dynamic system dependencies'
    for library in $libraries; do
        case "$library" in
            /usr/lib/*|/System/Library/Frameworks/*) ;;
            @rpath/libclang_rt.*)
                [ "$configuration" = sanitizers ] ||
                    fail "sanitizer runtime is not allowed for $configuration: $library"
                ;;
            *) fail "library is outside the macOS allowlist: $library" ;;
        esac
    done

    load_commands=$(otool -l "$binary") || fail 'otool could not read load commands'
    rpaths=$(printf '%s\n' "$load_commands" |
        awk '
            $1 == "cmd" && $2 == "LC_RPATH" { active = 1; next }
            active && $1 == "path" { print $2; active = 0 }
        ' | sort -u)
    if [ -n "$rpaths" ]; then
        [ "$configuration" = sanitizers ] || fail "LC_RPATH is not allowed: $(printf '%s' "$rpaths" | tr '\n' ' ')"
        printf '%s\n' "$rpaths" | while IFS= read -r path; do
            case "$path" in
                @executable_path|\
                *Xcode*.app*/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/*/lib/darwin)
                    ;;
                *)
                    fail "unexpected sanitizer LC_RPATH: $path"
                    ;;
            esac
        done
    fi
    minimum_os=$(printf '%s\n' "$load_commands" | awk '
        $1 == "cmd" && $2 == "LC_BUILD_VERSION" {
            mode = "build"
            next
        }
        $1 == "cmd" && $2 == "LC_VERSION_MIN_MACOSX" {
            mode = "legacy"
            next
        }
        mode == "build" && $1 == "minos" {
            print $2
            exit
        }
        mode == "legacy" && $1 == "version" {
            print $2
            exit
        }
    ')
    case "$minimum_os" in
        13.0|13.0.0)
            minimum_os=13.0
            ;;
        *)
            fail "Mach-O minimum macOS version '${minimum_os:-missing}' does not match policy 13.0"
            ;;
    esac

    symbols=$(nm -a "$binary" 2>/dev/null || true)
    case "$configuration" in
        debug)
            # macOS debug symbols are validated in the finalized dSYM.
            ;;
        coverage)
            printf '%s\n' "$symbols" | grep -E '___llvm_profile|__llvm_profile' >/dev/null ||
                fail 'coverage executable contains no LLVM coverage instrumentation'
            ;;
        sanitizers)
            printf '%s\n%s\n' "$symbols" "$libraries" |
                grep -E '__asan_init|libclang_rt.*asan' >/dev/null ||
                fail 'sanitizer executable contains no ASan runtime'
            printf '%s\n%s\n' "$symbols" "$libraries" |
                grep -E '__ubsan_handle_|libclang_rt.*ubsan' >/dev/null ||
                fail 'sanitizer executable contains no UBSan runtime'
            ;;
    esac
    if [ "$configuration" = release ] && [ "$inspection_policy" = public ]; then
        ! printf '%s\n' "$load_commands" | grep -E '__debug_info|__DWARF' >/dev/null ||
            fail 'public release executable still contains DWARF information'
    fi

    needed_count=$(printf '%s\n' "$libraries" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=2\nplatform=%s\nconfiguration=%s\ninspection_policy=%s\n' \
            "$platform" "$configuration" "$inspection_policy"
        printf 'binary=%s\nsha256=%s\nobject_format=Mach-O\narchitecture=%s\nminimum_os=%s\n' \
            "$(basename "$binary")" "$sha256" "$architecture" "$minimum_os"
        printf 'linkage=third-party-static-system-dynamic\n'
        printf 'third_party_linkage=static\nsystem_linkage=dynamic\nneeded_count=%s\n' \
            "$needed_count"
        printf '%s\n' "$libraries" | write_needed_entries
        printf 'runtime_search_path=%s\nfile_description=%s\n' "${rpaths:-none}" "$file_description"
    } | write_report
}

find_pe_objdump() {
    if [ -n "${CUP_OBJDUMP:-}" ]; then
        command -v "$CUP_OBJDUMP" >/dev/null 2>&1 || fail "configured objdump '$CUP_OBJDUMP' was not found"
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
        *PE32+*x86-64*|*PE32+*x86_64*)
            ;;
        *)
            fail "expected a PE32+ x86-64 executable, got: $file_description"
            ;;
    esac
    case "$file_description" in
        *DLL*)
            fail 'PE object is a DLL, not an executable'
            ;;
    esac
    objdump=$(find_pe_objdump)
    coff_header=$($objdump -f "$binary") || fail "$objdump could not read the COFF header"
    coff_architecture=$(printf '%s\n' "$coff_header" | awk '
        /^architecture:[[:space:]]*/ {
            sub(/^architecture:[[:space:]]*/, "")
            sub(/,.*/, "")
            print
            exit
        }
    ')
    case "$coff_architecture" in
        i386:x86-64|x86-64|x86_64|amd64) ;;
        *)
            fail "PE/COFF architecture does not match windows-x64: "\
                "${coff_architecture:-missing} ($objdump)"
            ;;
    esac
    pe_headers=$($objdump -p "$binary") || fail "$objdump could not read PE headers"
    printf '%s\n' "$pe_headers" |
        grep -Ei '^[[:space:]]*executable[[:space:]]*$' >/dev/null ||
        fail 'PE/COFF object is not executable'
    subsystem=$(printf '%s\n' "$pe_headers" | awk '
        $1 == "Subsystem" {
            sub(/^[[:space:]]*Subsystem[[:space:]]+[^[:space:]]+[[:space:]]*/, "")
            gsub(/[()]/, "")
            print
            exit
        }
    ')
    [ "$subsystem" = 'Windows CUI' ] || fail "unexpected PE subsystem: ${subsystem:-missing}"
    imports=$(printf '%s\n' "$pe_headers" | awk '$1 == "DLL" && $2 == "Name:" { print $3 }' | sort -fu)
    [ -n "$imports" ] || fail 'PE executable has no imported system DLLs'
    for library in $imports; do
        normalized=$(printf '%s' "$library" | tr '[:upper:]' '[:lower:]')
        case "$normalized" in
            advapi32.dll|bcrypt.dll|comctl32.dll|crypt32.dll|dnsapi.dll|gdi32.dll|\
            iphlpapi.dll|kernel32.dll|msvcrt.dll|ntdll.dll|ole32.dll|oleaut32.dll|\
            secur32.dll|shell32.dll|shlwapi.dll|user32.dll|version.dll|winhttp.dll|\
            winmm.dll|ws2_32.dll|ucrtbase.dll|api-ms-win-*.dll|ext-ms-win-*.dll)
                ;;
            libclang_rt.*.dll|clang_rt.*.dll)
                [ "$configuration" = sanitizers ] ||
                    fail "sanitizer runtime is not allowed for $configuration: $library"
                ;;
            *) fail "library is outside the Windows system DLL allowlist: $library" ;;
        esac
    done
    resource_line=$(printf '%s\n' "$pe_headers" | awk '/Resource Directory/ { print; exit }')
    [ -n "$resource_line" ] || fail 'PE executable has no resource directory'
    resource_size=$(printf '%s\n' "$resource_line" | awk '{ print $4 }')
    case "$resource_size" in
        ''|0|00000000|0000000000000000)
            fail 'PE resource directory is empty'
            ;;
    esac
    for hardening in DYNAMIC_BASE NX_COMPAT; do
        printf '%s\n' "$pe_headers" |
            grep -E "^[[:space:]]*$hardening([[:space:]]|$)" >/dev/null ||
            fail "PE executable is missing $hardening"
    done

    sections=$($objdump -h "$binary" 2>/dev/null || true)
    symbols=$($objdump -t "$binary" 2>/dev/null || true)
    case "$configuration" in
        debug)
            printf '%s\n' "$sections" | grep -E '\.debug_info' >/dev/null ||
                fail 'debug executable contains no DWARF information'
            ;;
        coverage)
            printf '%s\n' "$symbols" | grep -E '__gcov_(init|exit|merge)' >/dev/null ||
                fail 'coverage executable contains no gcov instrumentation'
            ;;
        sanitizers)
            printf '%s\n%s\n' "$symbols" "$imports" |
                grep -E '__asan_init|clang_rt.*asan' >/dev/null ||
                fail 'sanitizer executable contains no ASan runtime'
            printf '%s\n%s\n' "$symbols" "$imports" |
                grep -E '__ubsan_handle_|clang_rt.*ubsan' >/dev/null ||
                fail 'sanitizer executable contains no UBSan runtime'
            ;;
    esac
    if [ "$configuration" = release ] && [ "$inspection_policy" = public ]; then
        ! printf '%s\n' "$sections" | grep -E '\.debug_info' >/dev/null ||
            fail 'public release executable still contains DWARF information'
    fi

    needed_count=$(printf '%s\n' "$imports" | awk 'NF { count++ } END { print count + 0 }')
    {
        printf 'format=2\nplatform=%s\nconfiguration=%s\ninspection_policy=%s\n' \
            "$platform" "$configuration" "$inspection_policy"
        printf 'binary=%s\nsha256=%s\nobject_format=PE32+\narchitecture=x86_64\nsubsystem=%s\n' \
            "$(basename "$binary")" "$sha256" "$subsystem"
        printf 'linkage=dynamic-system\nneeded_count=%s\n' "$needed_count"
        printf '%s\n' "$imports" | write_needed_entries
        printf 'resource_directory=present\ndynamic_base=yes\nnx_compat=yes\n'
        printf 'runtime_search_path=none\nfile_description=%s\n' "$file_description"
    } | write_report
}

case "$platform" in
    linux-x64|linux-arm64)
        inspect_elf
        ;;
    macos-x64|macos-arm64)
        inspect_macho
        ;;
    windows-x64)
        inspect_pe
        ;;
    *)
        fail "unsupported platform '$platform'"
        ;;
esac

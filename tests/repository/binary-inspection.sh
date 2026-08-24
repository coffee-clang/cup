#!/bin/sh

# Verifies deterministic ELF, Mach-O and PE reports plus rejection of
# wrong architectures, third-party dynamic libraries and unsafe loader metadata.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin binary-inspection
fake_bin=$TMP_ROOT/bin
mkdir -p "$fake_bin"
binary=$TMP_ROOT/cup
printf 'fixture\n' >"$binary"
chmod +x "$binary"

cat >"$fake_bin/file" <<'EOF_FILE'
#!/bin/sh
case "${FAKE_FORMAT:-elf}" in
    elf)
        printf '%s\n' 'ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV)'
        ;;
    macho)
        printf '%s\n' 'Mach-O 64-bit executable x86_64'
        ;;
    pe)
        printf '%s\n' 'PE32+ executable (console) x86-64, for MS Windows'
        ;;
    *)
        printf '%s\n' 'ASCII text'
        ;;
esac
EOF_FILE

cat >"$fake_bin/readelf" <<'EOF_READELF'
#!/bin/sh
case "${1:-}" in
    -h)
        cat <<EOF_HEADER
ELF Header:
  Class:                             ELF64
  Data:                              2's complement, little endian
  Type:                              DYN (Position-Independent Executable file)
  Machine:                           ${FAKE_MACHINE:-Advanced Micro Devices X86-64}
  Entry point address:               0x1000
EOF_HEADER
        ;;
    -l)
        printf '%s\n' '  LOAD           0x000000 0x0000000000400000 0x0000000000400000'
        if [ "${FAKE_INTERPRETER:-1}" = 1 ]; then
            printf '%s\n' '      [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]'
        fi
        ;;
    -d)
        case "${FAKE_NEEDED:-libc.so.6}" in
            none)
                printf '%s\n' 'There is no dynamic section in this file.'
                ;;
            *)
                old_ifs=$IFS
                IFS=,
                for library in ${FAKE_NEEDED:-libc.so.6}; do
                    printf ' 0x0000000000000001 (NEEDED) Shared library: [%s]\n' "$library"
                done
                IFS=$old_ifs
                [ "${FAKE_RPATH:-0}" = 0 ] ||
                    printf '%s\n' ' 0x000000000000001d (RUNPATH) Library runpath: [/tmp/lib]'
                ;;
        esac
        ;;
    -S)
        printf '%s\n' 'Section Headers:' '  [ 1] .text PROGBITS'
        ;;
    -Ws)
        printf '%s\n' 'Symbol table:'
        ;;
    *)
        exit 2
        ;;
esac
EOF_READELF

cat >"$fake_bin/lipo" <<'EOF_LIPO'
#!/bin/sh
[ "${1:-}" = -archs ] || exit 2
printf '%s\n' "${FAKE_MAC_ARCH:-x86_64}"
EOF_LIPO

cat >"$fake_bin/otool" <<'EOF_OTOOL'
#!/bin/sh
case "${1:-}" in
    -hv)
        printf '%s\n' \
            'Mach header' \
            '      magic cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags' \
            'MH_MAGIC_64 X86_64 ALL 0x00 EXECUTE 12 1024 NOUNDEFS DYLDLINK TWOLEVEL PIE'
        ;;
    -L)
        printf '%s:\n' "$2"
        old_ifs=$IFS
        IFS=,
        default_libraries='/usr/lib/libSystem.B.dylib,'
        default_libraries=$default_libraries'/System/Library/Frameworks/'
        default_libraries=$default_libraries'Security.framework/Versions/A/Security'
        for library in ${FAKE_MAC_LIBS:-$default_libraries}; do
            printf '\t%s (compatibility version 1.0.0, current version 1.0.0)\n' "$library"
        done
        IFS=$old_ifs
        ;;
    -l)
        if [ "${FAKE_MAC_MINOS:-13.0}" != none ]; then
            cat <<EOF_LOAD
Load command 1
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos ${FAKE_MAC_MINOS:-13.0}
EOF_LOAD
        fi
        if [ "${FAKE_MAC_RPATH:-0}" != 0 ]; then
            rpath=${FAKE_MAC_RPATH}
            [ "$rpath" != 1 ] || rpath=/tmp/lib
            cat <<EOF_RPATH
Load command 2
          cmd LC_RPATH
      cmdsize 32
         path $rpath (offset 12)
EOF_RPATH
        fi
        ;;
    *)
        exit 2
        ;;
esac
EOF_OTOOL

cat >"$fake_bin/nm" <<'EOF_NM'
#!/bin/sh
printf '%s\n' "${FAKE_NM_SYMBOLS:-}"
EOF_NM

cat >"$fake_bin/x86_64-w64-mingw32-objdump" <<'EOF_OBJDUMP'
#!/bin/sh
tool=${0##*/}
case "${1:-}" in
    -f)
        if [ "$tool" = llvm-objdump ]; then
            cat <<EOF_HEADER
$2:	file format coff-x86-64
architecture: ${FAKE_PE_ARCH:-x86_64}
start address: 0x0000000000000000
EOF_HEADER
        else
            cat <<EOF_HEADER
$2:     file format pei-x86-64
architecture: ${FAKE_PE_ARCH:-i386:x86-64}, flags 0x0000012f:
HAS_RELOC, EXEC_P, HAS_LINENO, HAS_DEBUG, HAS_SYMS, HAS_LOCALS, D_PAGED
EOF_HEADER
        fi
        ;;
    -h|-t)
        exit 0
        ;;
    -p)
        printf 'Characteristics 0x22\n'
        [ "${FAKE_PE_EXECUTABLE:-1}" = 1 ] && printf '\texecutable\n'
        printf '\tlarge address aware\n'
        printf 'Subsystem\t\t00000003 (%s)\n' "${FAKE_PE_SUBSYSTEM:-Windows CUI}"
        old_ifs=$IFS
        IFS=,
        for library in ${FAKE_PE_DLLS:-KERNEL32.dll,WS2_32.dll}; do
            printf '\tDLL Name: %s\n' "$library"
        done
        IFS=$old_ifs
        if [ "${FAKE_PE_RESOURCE:-1}" = 1 ]; then
            printf 'Entry 2 0000000000005000 00000200 Resource Directory [.rsrc]\n'
        fi
        [ "${FAKE_PE_DYNAMIC_BASE:-1}" = 1 ] && printf '\tDYNAMIC_BASE\n'
        [ "${FAKE_PE_NX:-1}" = 1 ] && printf '\tNX_COMPAT\n'
        exit 0
        ;;
    *)
        exit 2
        ;;
esac
EOF_OBJDUMP
cp "$fake_bin/x86_64-w64-mingw32-objdump" "$fake_bin/llvm-objdump"
chmod +x "$fake_bin/file" "$fake_bin/readelf" "$fake_bin/lipo" \
    "$fake_bin/otool" "$fake_bin/nm" "$fake_bin/x86_64-w64-mingw32-objdump" \
    "$fake_bin/llvm-objdump"

inspect() {
    PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/inspect-binary.sh" "$@"
}

# ELF contract.
development_report=$TMP_ROOT/development.txt
inspect linux-x64 development "$binary" "$development_report"
text=$(cat "$development_report")
assert_contains "$text" 'object_format=ELF'
assert_contains "$text" 'architecture=x86_64'
assert_contains "$text" 'linkage=dynamic-system'
assert_contains "$text" 'needed=libc.so.6'
assert_contains "$text" 'runtime_search_path=none'

release_report=$TMP_ROOT/release.txt
FAKE_NEEDED=none FAKE_INTERPRETER=0 inspect \
    linux-x64 release "$binary" "$release_report"
assert_contains "$(cat "$release_report")" 'linkage=static'
assert_contains "$(cat "$release_report")" 'needed_count=0'

if FAKE_NEEDED=libc.so.6,libcurl.so.4 inspect \
        linux-x64 development "$binary" "$TMP_ROOT/third-party.txt" \
        >"$TMP_ROOT/third-party.out" 2>&1; then
    fail 'dynamic libcurl dependency was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/third-party.out")" \
    'library is outside the Linux system/runtime allowlist: libcurl.so.4'

if FAKE_NEEDED=none inspect linux-x64 release "$binary" \
        "$TMP_ROOT/interpreter.txt" >"$TMP_ROOT/interpreter.out" 2>&1; then
    fail 'release interpreter was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/interpreter.out")" \
    'release executable has a dynamic interpreter'

if FAKE_MACHINE=AArch64 inspect linux-x64 development "$binary" \
        "$TMP_ROOT/machine.txt" >"$TMP_ROOT/machine.out" 2>&1; then
    fail 'wrong ELF machine was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/machine.out")" 'does not match linux-x64'

if FAKE_RPATH=1 inspect linux-x64 development "$binary" \
        "$TMP_ROOT/rpath.txt" >"$TMP_ROOT/rpath.out" 2>&1; then
    fail 'ELF RUNPATH was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/rpath.out")" 'RPATH/RUNPATH is not allowed'

# Mach-O contract.
mac_report=$TMP_ROOT/macos.txt
FAKE_FORMAT=macho inspect macos-x64 release "$binary" "$mac_report"
mac_text=$(cat "$mac_report")
assert_contains "$mac_text" 'object_format=Mach-O'
# Mach-O debug evidence is validated through the finalized dSYM.
mac_debug_report=$TMP_ROOT/macos-debug.txt
FAKE_FORMAT=macho inspect macos-x64 debug "$binary" "$mac_debug_report"
assert_contains "$(cat "$mac_debug_report")" 'configuration=debug'
assert_contains "$mac_text" 'architecture=x86_64'
assert_contains "$mac_text" 'minimum_os=13.0'
assert_contains "$mac_text" 'linkage=third-party-static-system-dynamic'
assert_contains "$mac_text" 'third_party_linkage=static'
assert_contains "$mac_text" 'system_linkage=dynamic'
assert_contains "$mac_text" 'needed=/usr/lib/libSystem.B.dylib'

arm_report=$TMP_ROOT/macos-arm64.txt
FAKE_FORMAT=macho FAKE_MAC_ARCH=arm64 inspect \
    macos-arm64 development "$binary" "$arm_report"
assert_contains "$(cat "$arm_report")" 'architecture=arm64'

if FAKE_FORMAT=macho FAKE_MAC_LIBS=/opt/homebrew/lib/libcurl.4.dylib inspect \
        macos-x64 development "$binary" "$TMP_ROOT/homebrew.txt" \
        >"$TMP_ROOT/homebrew.out" 2>&1; then
    fail 'Homebrew Mach-O dependency was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/homebrew.out")" \
    'library is outside the macOS allowlist'

if FAKE_FORMAT=macho FAKE_MAC_ARCH=arm64 inspect macos-x64 development \
        "$binary" "$TMP_ROOT/mac-arch.txt" >"$TMP_ROOT/mac-arch.out" 2>&1; then
    fail 'wrong Mach-O architecture was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/mac-arch.out")" 'do not match macos-x64'

if FAKE_FORMAT=macho FAKE_MAC_RPATH=1 inspect macos-x64 development \
        "$binary" "$TMP_ROOT/mac-rpath.txt" >"$TMP_ROOT/mac-rpath.out" 2>&1; then
    fail 'Mach-O LC_RPATH was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/mac-rpath.out")" 'LC_RPATH is not allowed'

if FAKE_FORMAT=macho FAKE_MAC_MINOS=none inspect macos-x64 development \
        "$binary" "$TMP_ROOT/mac-minos.txt" >"$TMP_ROOT/mac-minos.out" 2>&1; then
    fail 'Mach-O binary without minimum OS was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/mac-minos.out")" "minimum macOS version 'missing' does not match policy 13.0"

if FAKE_FORMAT=macho FAKE_MAC_MINOS=12.0 inspect macos-x64 development \
        "$binary" "$TMP_ROOT/mac-floor.txt" >"$TMP_ROOT/mac-floor.out" 2>&1; then
    fail 'Mach-O binary below the supported deployment target was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/mac-floor.out")" \
    "minimum macOS version '12.0' does not match policy 13.0"

mac_patch_report=$TMP_ROOT/macos-patch.txt
FAKE_FORMAT=macho FAKE_MAC_MINOS=13.0.0 inspect \
    macos-x64 development "$binary" "$mac_patch_report"
assert_contains "$(cat "$mac_patch_report")" 'minimum_os=13.0'

# PE/COFF contract.
pe_report=$TMP_ROOT/windows.txt
FAKE_FORMAT=pe inspect windows-x64 release "$binary" "$pe_report"
pe_text=$(cat "$pe_report")
assert_contains "$pe_text" 'object_format=PE32+'
assert_contains "$pe_text" 'architecture=x86_64'
assert_contains "$pe_text" 'subsystem=Windows CUI'
assert_contains "$pe_text" 'needed=KERNEL32.dll'
assert_contains "$pe_text" 'resource_directory=present'
assert_contains "$pe_text" 'dynamic_base=yes'
assert_contains "$pe_text" 'nx_compat=yes'

# GNU objdump reports i386:x86-64 while LLVM objdump reports x86_64 for the
# same COFF machine. Both are valid x86-64 evidence and must pass the same gate.
llvm_pe_report=$TMP_ROOT/windows-llvm-objdump.txt
CUP_OBJDUMP=llvm-objdump FAKE_FORMAT=pe inspect \
    windows-x64 release "$binary" "$llvm_pe_report"
assert_contains "$(cat "$llvm_pe_report")" 'architecture=x86_64'

if FAKE_FORMAT=pe FAKE_PE_DLLS=KERNEL32.dll,libgcc_s_seh-1.dll inspect \
        windows-x64 development "$binary" "$TMP_ROOT/pe-runtime.txt" \
        >"$TMP_ROOT/pe-runtime.out" 2>&1; then
    fail 'MinGW runtime DLL was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-runtime.out")" \
    'library is outside the Windows system DLL allowlist: libgcc_s_seh-1.dll'

if FAKE_FORMAT=pe FAKE_PE_ARCH=i386 inspect windows-x64 development \
        "$binary" "$TMP_ROOT/pe-arch.txt" >"$TMP_ROOT/pe-arch.out" 2>&1; then
    fail 'wrong PE architecture was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-arch.out")" 'architecture does not match windows-x64'

if FAKE_FORMAT=pe FAKE_PE_EXECUTABLE=0 inspect windows-x64 development \
        "$binary" "$TMP_ROOT/pe-executable.txt" >"$TMP_ROOT/pe-executable.out" 2>&1; then
    fail 'non-executable PE object was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-executable.out")" 'PE/COFF object is not executable'

if FAKE_FORMAT=pe FAKE_PE_RESOURCE=0 inspect windows-x64 development \
        "$binary" "$TMP_ROOT/pe-resource.txt" >"$TMP_ROOT/pe-resource.out" 2>&1; then
    fail 'PE binary without resources was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-resource.out")" 'has no resource directory'

if FAKE_FORMAT=pe FAKE_PE_NX=0 inspect windows-x64 development \
        "$binary" "$TMP_ROOT/pe-nx.txt" >"$TMP_ROOT/pe-nx.out" 2>&1; then
    fail 'PE binary without NX compatibility was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-nx.out")" 'is missing NX_COMPAT'


# Sanitizer inspection requires evidence for both AddressSanitizer and
# UndefinedBehaviorSanitizer on every supported object format.
FAKE_FORMAT=macho \
FAKE_MAC_LIBS='@rpath/libclang_rt.asan_osx_dynamic.dylib' \
FAKE_MAC_RPATH='@executable_path' \
FAKE_NM_SYMBOLS='__asan_init __ubsan_handle_type_mismatch_v1' \
    inspect macos-x64 sanitizers "$binary" "$TMP_ROOT/mac-sanitizers.txt"
if FAKE_FORMAT=macho \
        FAKE_MAC_LIBS='@rpath/libclang_rt.asan_osx_dynamic.dylib' \
        FAKE_MAC_RPATH='/tmp/lib' \
        FAKE_NM_SYMBOLS='__asan_init __ubsan_handle_type_mismatch_v1' \
        inspect macos-x64 sanitizers "$binary" "$TMP_ROOT/mac-sanitizer-rpath.txt" \
        >"$TMP_ROOT/mac-sanitizer-rpath.out" 2>&1; then
    fail 'Mach-O sanitizer binary accepted an arbitrary LC_RPATH'
fi
assert_contains "$(cat "$TMP_ROOT/mac-sanitizer-rpath.out")" \
    'unexpected sanitizer LC_RPATH: /tmp/lib'

if FAKE_FORMAT=macho \
        FAKE_MAC_LIBS='@rpath/libclang_rt.asan_osx_dynamic.dylib' \
        FAKE_NM_SYMBOLS='__asan_init' \
        inspect macos-x64 sanitizers "$binary" "$TMP_ROOT/mac-no-ubsan.txt" \
        >"$TMP_ROOT/mac-no-ubsan.out" 2>&1; then
    fail 'Mach-O sanitizer binary without UBSan was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/mac-no-ubsan.out")" 'contains no UBSan runtime'

FAKE_FORMAT=pe \
FAKE_PE_DLLS='KERNEL32.dll,clang_rt.asan_dynamic-x86_64.dll,clang_rt.ubsan_standalone-x86_64.dll' \
    inspect windows-x64 sanitizers "$binary" "$TMP_ROOT/pe-sanitizers.txt"
if FAKE_FORMAT=pe \
        FAKE_PE_DLLS='KERNEL32.dll,clang_rt.asan_dynamic-x86_64.dll' \
        inspect windows-x64 sanitizers "$binary" "$TMP_ROOT/pe-no-ubsan.txt" \
        >"$TMP_ROOT/pe-no-ubsan.out" 2>&1; then
    fail 'PE sanitizer binary without UBSan was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/pe-no-ubsan.out")" 'contains no UBSan runtime'

if [ "${CUP_TEST_WITH_BUILD_OUTPUT:-0}" = 1 ] && [ "$(uname -s)" = Linux ]; then
    test_build_root=${CUP_TEST_BUILD_ROOT:-$PROJECT_ROOT/build}
    real_development="$test_build_root/linux-x64/development/bin/cup"
    real_release="$test_build_root/linux-x64/release/bin/cup"
    [ -x "$real_development" ] &&
        "$PROJECT_ROOT/scripts/build/inspect-binary.sh" linux-x64 development \
            "$real_development" "$TMP_ROOT/real-development.txt"
    [ -x "$real_release" ] &&
        "$PROJECT_ROOT/scripts/build/inspect-binary.sh" linux-x64 release \
            "$real_release" "$TMP_ROOT/real-release.txt"
fi

printf '%s\n' 'Binary-inspection contract tests passed.'

# Release path-leak guard must reject runner/staging paths and accept the
# intentional neutral OpenSSL namespace.
path_guard="$PROJECT_ROOT/scripts/build/check-path-leaks.sh"
printf '%s\n' 'OPENSSLDIR: "/__cup_runtime__/openssl"' > "$TMP_ROOT/neutral-binary"
"$path_guard" "$TMP_ROOT/neutral-binary" "$TMP_ROOT/forbidden"
printf '%s\n' "$TMP_ROOT/forbidden/file.c" > "$TMP_ROOT/leaking-binary"
if "$path_guard" "$TMP_ROOT/leaking-binary" "$TMP_ROOT/forbidden" \
        >"$TMP_ROOT/path-leak.out" 2>&1; then
    fail 'release path guard accepted a machine-specific path'
fi
assert_contains "$(cat "$TMP_ROOT/path-leak.out")" 'contains forbidden path'
printf '%s\n' '/tmp/.install.staging.ABCD/include' > "$TMP_ROOT/staging-binary"
if "$path_guard" "$TMP_ROOT/staging-binary" \
        >"$TMP_ROOT/staging-leak.out" 2>&1; then
    fail 'release path guard accepted a staging path'
fi
assert_contains "$(cat "$TMP_ROOT/staging-leak.out")" 'transactional dependency path'
printf '%s\n' 'OPENSSLDIR: "/__cup_runtime__/other"' > "$TMP_ROOT/wrong-neutral-binary"
if "$path_guard" "$TMP_ROOT/wrong-neutral-binary" \
        >"$TMP_ROOT/wrong-neutral.out" 2>&1; then
    fail 'release path guard accepted an unexpected neutral namespace'
fi
assert_contains "$(cat "$TMP_ROOT/wrong-neutral.out")" 'unexpected neutral runtime namespace'

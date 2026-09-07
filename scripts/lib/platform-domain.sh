
# Owns the repository platform domain and native uname-to-platform mapping for shell tooling.

CUP_SUPPORTED_PLATFORMS='linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64'

cup_platform_valid() (
    value=${1-}
    for candidate in $CUP_SUPPORTED_PLATFORMS; do
        [ "$value" = "$candidate" ] && return 0
    done
    return 1
)

cup_platform_from_uname() (
    system=${1-}
    machine=${2-}

    case "$system:$machine" in
        Linux:x86_64|Linux:amd64) printf '%s\n' linux-x64 ;;
        Linux:aarch64|Linux:arm64) printf '%s\n' linux-arm64 ;;
        Darwin:x86_64|Darwin:amd64) printf '%s\n' macos-x64 ;;
        Darwin:arm64|Darwin:aarch64) printf '%s\n' macos-arm64 ;;
        MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) printf '%s\n' windows-x64 ;;
        *) return 1 ;;
    esac
)

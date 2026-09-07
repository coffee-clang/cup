
# Owns the repository-wide build-configuration domain used by POSIX tooling.

CUP_BUILD_CONFIGURATIONS='development debug coverage sanitizers release'

cup_build_configuration_valid() (
    value=${1-}
    for candidate in $CUP_BUILD_CONFIGURATIONS; do
        [ "$value" = "$candidate" ] && return 0
    done
    return 1
)

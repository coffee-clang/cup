# Loads the dependency modules shared by native builders and prefix verification.
# This library is sourced by build-posix.sh, build-windows.sh and verify.sh and
# is intentionally not executable.

CUP_DEPENDENCIES_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CUP_PROJECT_ROOT="$(CDPATH= cd -- "$CUP_DEPENDENCIES_DIR/../.." && pwd)"
PROJECT_ROOT=$CUP_PROJECT_ROOT

# shellcheck source=sources.sh
source "$CUP_DEPENDENCIES_DIR/sources.sh"
# shellcheck source=../lib/path-safety.sh
source "$CUP_DEPENDENCIES_DIR/../lib/path-safety.sh"

CUP_DEPENDENCY_PREFIX_FORMAT=5
CUP_DEPENDENCY_ROOT_MARKER=.cup-dependencies-root


# Each sourced module owns one distinct part of dependency preparation.
# shellcheck source=environment.sh
source "$CUP_DEPENDENCIES_DIR/environment.sh"
# shellcheck source=root-transaction.sh
source "$CUP_DEPENDENCIES_DIR/root-transaction.sh"
# shellcheck source=prefix-metadata.sh
source "$CUP_DEPENDENCIES_DIR/prefix-metadata.sh"
# shellcheck source=source-build.sh
source "$CUP_DEPENDENCIES_DIR/source-build.sh"

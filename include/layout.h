#ifndef CUP_LAYOUT_H
#define CUP_LAYOUT_H

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "package.h"

typedef enum {
    LAYOUT_RUNTIME_MISSING,
    LAYOUT_RUNTIME_READY,
    LAYOUT_RUNTIME_INCOMPLETE
} LayoutRuntimeStatus;

/* Canonical paths inside ~/.cup. */
CupError layout_get_root(char *buffer, size_t size);
CupError layout_get_bin_dir(char *buffer, size_t size);
CupError layout_get_components_dir(char *buffer, size_t size);
CupError layout_get_tmp_dir(char *buffer, size_t size);
CupError layout_get_state_path(char *buffer, size_t size);
CupError layout_get_manifest_path(char *buffer, size_t size);
CupError layout_get_common_checksums_path(char *buffer, size_t size);
CupError layout_get_platform_checksums_path(char *buffer, size_t size);
CupError layout_get_uninstall_path(char *buffer, size_t size);
CupError layout_get_lock_path(char *buffer, size_t size);
CupError layout_get_transaction_path(char *buffer, size_t size);
CupError layout_get_uninstall_marker_path(char *buffer, size_t size);
CupError layout_get_binary_path(char *buffer, size_t size);

/* Paths derived from a concrete package identity. */
CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity);
CupError layout_build_cache_archive_path(char *buffer, size_t size,
    const PackageIdentity *identity, const char *format);

/* Inspect or create the bootstrap and runtime portions of ~/.cup. */
CupError layout_get_runtime_status(LayoutRuntimeStatus *status);
CupError layout_check_runtime(size_t *missing_count);
CupError layout_ensure_root(void);
CupError layout_ensure_runtime(void);
CupError layout_ensure_bootstrap(void);
CupError layout_ensure_package_parent(const PackageIdentity *identity);
CupError layout_ensure_cache_parent(const PackageIdentity *identity);
CupError layout_create_tmp_dir(char *buffer, size_t size,
    const char *operation, const PackageIdentity *identity);
CupError layout_make_tmp_path(char *buffer, size_t size,
    const char *operation, const PackageIdentity *identity);
CupError layout_build_tmp_prefix(char *buffer, size_t size,
    const char *operation, const PackageIdentity *identity);
CupError layout_create_recovery_dir(char *buffer, size_t size,
    const PackageIdentity *identity);

#endif /* CUP_LAYOUT_H */

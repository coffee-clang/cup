#ifndef CUP_LAYOUT_H
#define CUP_LAYOUT_H

/*
 * Module contract: Canonical .cup paths and creation of CUP assets, runtime,
 * staging, cache, and recovery directories. No caller may invent a managed
 * path independently of this module.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "package.h"

typedef enum {
    LAYOUT_RUNTIME_MISSING,
    LAYOUT_RUNTIME_READY,
    LAYOUT_RUNTIME_INCOMPLETE
} LayoutRuntimeStatus;

/* Canonical paths inside the current user's ~/.cup root. */
CupError layout_get_root(char *buffer, size_t size);
CupError layout_get_bin_dir(char *buffer, size_t size);
CupError layout_get_components_dir(char *buffer, size_t size);
CupError layout_get_staging_dir(char *buffer, size_t size);
CupError layout_get_config_dir(char *buffer, size_t size);
CupError layout_get_state_path(char *buffer, size_t size);
CupError layout_get_package_catalog_path(char *buffer, size_t size);
CupError layout_get_install_policy_path(char *buffer, size_t size);
CupError layout_get_preferences_path(char *buffer, size_t size);
CupError layout_get_common_checksums_path(char *buffer, size_t size);
CupError layout_get_platform_checksums_path(char *buffer, size_t size);
CupError layout_get_uninstall_path(char *buffer, size_t size);
CupError layout_get_lock_path(char *buffer, size_t size);
CupError layout_get_transaction_path(char *buffer, size_t size);
CupError layout_get_cup_update_result_path(char *buffer, size_t size);
CupError layout_get_cup_update_helper_path(char *buffer, size_t size);
CupError layout_get_uninstall_marker_path(char *buffer, size_t size);
CupError layout_get_binary_path(char *buffer, size_t size);

/* Canonical paths derived from one already validated package identity. */
CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity);
CupError layout_build_cache_archive_path(char *buffer,
                                         size_t size,
                                         const PackageIdentity *identity,
                                         const char *format);

/* Inspect the runtime tree without creating or modifying it. */
CupError layout_get_runtime_status(LayoutRuntimeStatus *status);
CupError layout_check_runtime(size_t *missing_count);

/* Create canonical portions of the managed tree idempotently. */
CupError layout_ensure_root(void);
CupError layout_ensure_runtime(void);
CupError layout_ensure_config(void);
CupError layout_ensure_cup_assets(void);
CupError layout_ensure_package_parent(const PackageIdentity *identity);
CupError layout_ensure_cache_parent(const PackageIdentity *identity);

/* Build or create unique staging and recovery locations below ~/.cup. */
CupError layout_create_staging_dir(char *buffer,
                                   size_t size,
                                   const char *operation,
                                   const PackageIdentity *identity);
CupError layout_make_staging_path(char *buffer,
                                  size_t size,
                                  const char *operation,
                                  const PackageIdentity *identity);
CupError layout_build_staging_prefix(char *buffer,
                                     size_t size,
                                     const char *operation,
                                     const PackageIdentity *identity);
CupError layout_create_recovery_dir(char *buffer, size_t size, const PackageIdentity *identity);

#endif /* CUP_LAYOUT_H */

#ifndef CUP_LAYOUT_H
#define CUP_LAYOUT_H

/*
 * Canonical paths below the selected cup root and creation of asset, runtime, staging,
 * cache, and recovery directories. Callers must not construct managed paths independently.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "package.h"
#include "system.h"

typedef enum {
    LAYOUT_RUNTIME_MISSING,
    /* Required runtime entries have the expected kinds and the selected root is private. */
    LAYOUT_RUNTIME_READY,
    LAYOUT_RUNTIME_INCOMPLETE
} LayoutRuntimeStatus;

/*
 * Freeze the selected root for one public command. Nested users share the same snapshot;
 * the final end releases it. Existing roots are identity-bound immediately. A selected missing
 * root may only be created exclusively; successful creation pins its new identity.
 */
CupError layout_root_snapshot_begin(void);
CupError layout_root_snapshot_validate(void);
void layout_root_snapshot_end(void);

/* Canonical paths inside the selected current-user cup root. */
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
CupError layout_get_lock_path(char *buffer, size_t size);
CupError layout_build_lock_path(char *buffer, size_t size, const char *root);
CupError layout_build_transaction_path(char *buffer, size_t size, const char *root);
/* Validate one explicit current-user managed root without normal handoff admission. Internal
 * helpers use this only after accepting inherited exclusive authority. */
CupError layout_validate_root_at(const char *root, SystemPathIdentity *identity);
CupError layout_get_transaction_path(char *buffer, size_t size);
CupError layout_get_update_helper_path(char *buffer, size_t size);
CupError layout_get_binary_path(char *buffer, size_t size);

/* Canonical paths derived from one already validated package identity. */
CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity);
CupError layout_build_cache_archive_path(char *buffer,
                                         size_t size,
                                         const PackageIdentity *identity,
                                         const char *format);

/* Inspect the runtime tree without creating or modifying it. */
CupError layout_check_root_candidates(size_t *issue_count);
CupError layout_get_runtime_status(LayoutRuntimeStatus *status);
CupError layout_check_runtime(size_t *missing_count);

/* Create canonical portions of the managed tree idempotently. */
CupError layout_ensure_root(void);
CupError layout_ensure_runtime(void);
CupError layout_ensure_config(void);
CupError layout_ensure_assets(void);
CupError layout_ensure_package_parent(const PackageIdentity *identity);
CupError layout_ensure_cache_parent(const PackageIdentity *identity);

/* Build or create unique staging and recovery locations below the selected root. */
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

#ifndef CUP_LAYOUT_H
#define CUP_LAYOUT_H

#include <stddef.h>

#include "error.h"
#include "package.h"

#define CUP_BIN_DIR "bin"
#define CUP_COMPONENTS_DIR "components"
#define CUP_TMP_DIR "tmp"
#define CUP_CACHE_DIR "cache"
#define CUP_CONFIG_DIR "config"
#define CUP_SCRIPTS_DIR "scripts"
#define CUP_STATE_FILE "state.txt"
#define CUP_LOCK_FILE "cup.lock"
#define CUP_TRANSACTION_FILE "transaction.txt"
#define CUP_MANIFEST_FILE "packages.cfg"
#define CUP_INFO_FILE "info.txt"

typedef enum {
    LAYOUT_RUNTIME_MISSING,
    LAYOUT_RUNTIME_READY,
    LAYOUT_RUNTIME_INCOMPLETE
} LayoutRuntimeStatus;

/* Canonical paths inside ~/.cup. */
CupError layout_get_root(char *buffer, size_t size);
CupError layout_get_components_dir(char *buffer, size_t size);
CupError layout_get_tmp_dir(char *buffer, size_t size);
CupError layout_get_state_path(char *buffer, size_t size);
CupError layout_get_manifest_path(char *buffer, size_t size);
CupError layout_get_uninstall_path(char *buffer, size_t size);
CupError layout_get_lock_path(char *buffer, size_t size);
CupError layout_get_transaction_path(char *buffer, size_t size);
CupError layout_get_binary_path(char *buffer, size_t size);

/* Paths derived from a concrete package identity. */
CupError layout_build_install_path(char *buffer, size_t size, const PackageIdentity *identity);
CupError layout_build_cache_archive_path(char *buffer, size_t size,
    const PackageIdentity *identity, const char *format);
CupError layout_build_tmp_path(char *buffer, size_t size, const char *operation,
    const PackageIdentity *identity, const char *suffix);

/* Inspect or create the bootstrap and runtime portions of ~/.cup. */
CupError layout_get_runtime_status(LayoutRuntimeStatus *status);
CupError layout_check_runtime(size_t *missing_count);
CupError layout_ensure_root(void);
CupError layout_ensure_runtime(void);
CupError layout_ensure_bootstrap(void);
CupError layout_ensure_package_parent(const PackageIdentity *identity);
CupError layout_ensure_cache_parent(const PackageIdentity *identity);
CupError layout_create_tmp_dir(char *buffer, size_t size, const char *operation, const PackageIdentity *identity);

#endif /* CUP_LAYOUT_H */

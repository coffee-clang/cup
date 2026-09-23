/* Runtime catalog refresh: snapshot, network outside the lock, validate, then CAS commit. */

#include "catalog_refresh.h"

#include "command_context.h"
#include "download.h"
#include "filesystem.h"
#include "layout.h"
#include "package_catalog.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define CATALOG_REFRESH_MAX_ATTEMPTS 3u

typedef struct {
    const unsigned char *data;
    size_t size;
} CatalogBytes;

static CupError write_catalog_bytes(FILE *file, const void *value) {
    const CatalogBytes *bytes = value;

    if (file == NULL || bytes == NULL || (bytes->size > 0 && bytes->data == NULL)) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (bytes->size > 0 && fwrite(bytes->data, 1, bytes->size, file) != bytes->size) {
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static int same_snapshot(const PackageCatalog *left, const PackageCatalog *right) {
    return left != NULL && right != NULL && left->revision == right->revision &&
           strcmp(left->digest, right->digest) == 0 &&
           system_path_identity_equal(&left->identity, &right->identity);
}

static CupError load_locked_catalog(PackageCatalog *catalog, SystemLockMode mode) {
    CommandContext context;
    CupError err;

    err = mode == SYSTEM_LOCK_SHARED
              ? command_context_begin_read_only(&context, NULL)
              : command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }
    if (!context.runtime_available) {
        command_context_end(&context);
        return CUP_ERR_NOT_INSTALLED;
    }
    err = package_catalog_load_installed(catalog);
    command_context_end(&context);
    return err;
}

static CupError commit_remote_catalog(const PackageCatalog *before,
                                      const PackageCatalog *remote,
                                      const PersistentFileSnapshot *remote_bytes,
                                      int *updated,
                                      int *retry) {
    CommandContext context;
    PackageCatalog current;
    CatalogBytes bytes;
    char config[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    CupError err;

    package_catalog_init(&current);
    *retry = 0;

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        return err;
    }
    err = package_catalog_load_installed(&current);
    if (err != CUP_OK) {
        goto done;
    }

    /* Another refresher may already have committed exactly these bytes while network I/O ran. */
    if (current.revision == remote->revision && strcmp(current.digest, remote->digest) == 0) {
        err = CUP_OK;
        goto done;
    }

    /* The local snapshot changed under us. Fetch again from the new snapshot/update URL rather
     * than comparing a response obtained from stale local authority. */
    if (!same_snapshot(before, &current)) {
        *retry = 1;
        err = CUP_OK;
        goto done;
    }

    if (remote->revision < current.revision) {
        fprintf(stderr,
                "Error: catalog refresh refused rollback from revision %llu to %llu.\n",
                (unsigned long long)current.revision,
                (unsigned long long)remote->revision);
        err = CUP_ERR_CATALOG;
        goto done;
    }
    if (remote->revision == current.revision) {
        fprintf(stderr,
                "Error: catalog revision %llu was published with different bytes.\n",
                (unsigned long long)current.revision);
        err = CUP_ERR_CATALOG;
        goto done;
    }

    err = layout_get_config_dir(config, sizeof(config));
    if (err == CUP_OK) {
        err = layout_get_package_catalog_path(path, sizeof(path));
    }
    if (err != CUP_OK) {
        goto done;
    }

    bytes.data = remote_bytes->data;
    bytes.size = remote_bytes->size;
    err = filesystem_replace_file_if_identity(
        config, "catalog", path, &current.identity, 0, write_catalog_bytes, &bytes);
    if (err == CUP_OK) {
        *updated = 1;
    }

done:
    package_catalog_free(&current);
    command_context_end(&context);
    return err;
}

CupError catalog_refresh_existing(int *updated, CatalogRefreshDiagnostics diagnostics) {
    unsigned attempt;

    if (updated == NULL ||
        (diagnostics != CATALOG_REFRESH_REPORT_ERRORS && diagnostics != CATALOG_REFRESH_QUIET)) {
        return CUP_ERR_INVALID_INPUT;
    }
    *updated = 0;

    for (attempt = 0; attempt < CATALOG_REFRESH_MAX_ATTEMPTS; ++attempt) {
        PackageCatalog before;
        PackageCatalog remote;
        PersistentFileSnapshot remote_bytes;
        char config[MAX_PATH_LEN];
        char temporary[MAX_PATH_LEN];
        CupError err;
        int missing = 0;
        int retry = 0;

        package_catalog_init(&before);
        package_catalog_init(&remote);
        filesystem_snapshot_init(&remote_bytes);
        temporary[0] = '\0';

        err = load_locked_catalog(&before, SYSTEM_LOCK_SHARED);
        if (err != CUP_OK) {
            goto attempt_done;
        }
        err = layout_get_config_dir(config, sizeof(config));
        if (err == CUP_OK) {
            err = system_make_unique_temp_path(
                config, "catalog-refresh", temporary, sizeof(temporary));
        }
        if (err == CUP_OK) {
            err = download_file_with_diagnostics(
                before.update_url,
                temporary,
                DOWNLOAD_VALIDATE_METADATA,
                diagnostics == CATALOG_REFRESH_REPORT_ERRORS
                    ? DOWNLOAD_DIAGNOSTICS_REPORT
                    : DOWNLOAD_DIAGNOSTICS_QUIET);
        }
        if (err == CUP_OK) {
            err = package_catalog_load_path(&remote, temporary);
        }
        if (err == CUP_OK) {
            err = filesystem_snapshot_read(
                temporary, MAX_PERSISTENT_METADATA_BYTES, &remote_bytes, &missing);
            if (err == CUP_OK && missing) {
                err = CUP_ERR_FILESYSTEM;
            }
        }
        if (err == CUP_OK) {
            err = commit_remote_catalog(&before, &remote, &remote_bytes, updated, &retry);
        }

attempt_done:
        if (temporary[0] != '\0') {
            (void)system_remove_file(temporary);
        }
        filesystem_snapshot_release(&remote_bytes);
        package_catalog_free(&remote);
        package_catalog_free(&before);

        if (err != CUP_OK) {
            return err;
        }
        if (!retry) {
            return CUP_OK;
        }
    }

    if (diagnostics == CATALOG_REFRESH_REPORT_ERRORS) {
        fprintf(stderr, "Error: catalog changed repeatedly while refresh was in progress.\n");
    }
    return CUP_ERR_TEMPORARY;
}

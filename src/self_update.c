#include "commands.h"

#include "bootstrap.h"
#include "checksum.h"
#include "command_context.h"
#include "fetch.h"
#include "layout.h"
#include "system.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

static CupError create_download_path(const char *prefix,
    char *path, size_t path_size) {
    char tmp_dir[MAX_PATH_LEN];
    FILE *file = NULL;
    CupError err;

    if (layout_get_tmp_dir(tmp_dir, sizeof(tmp_dir)) != CUP_OK) {
        return CUP_ERR_FILESYSTEM;
    }
    err = system_create_temp_file(tmp_dir, prefix, path, path_size, &file);
    if (err != CUP_OK) {
        return err;
    }
    if (fclose(file) != 0) {
        system_remove_file(path);
        return CUP_ERR_FILESYSTEM;
    }
    return CUP_OK;
}

static CupError installed_asset_matches(const char *checksums,
    const char *asset_name, const char *path, int *matches) {
    CupError err;
    int regular;

    if (matches == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    *matches = 0;

    err = system_is_regular_file(path, &regular);
    if (err != CUP_OK || !regular) {
        return err;
    }
    return bootstrap_verify_asset(checksums, asset_name, path, matches);
}

static CupError verify_downloaded_asset(const char *checksums,
    const char *asset_name, const char *path) {
    CupError err;
    int matches;

    err = bootstrap_verify_asset(checksums, asset_name, path, &matches);
    if (err != CUP_OK) {
        return err;
    }
    if (!matches) {
        fprintf(stderr,
            "Error: downloaded asset '%s' failed checksum verification.\n",
            asset_name);
        return CUP_ERR_VALIDATION;
    }
    return CUP_OK;
}

CupError command_self_update(void) {
    CommandContext context = {0};
    CupError err;
    char binary_name[MAX_NAME_LEN];
    char checksums_name[MAX_NAME_LEN];
    char installed_binary[MAX_PATH_LEN];
    char installed_uninstall[MAX_PATH_LEN];
    char installed_checksums[MAX_PATH_LEN];
    char staged_binary[MAX_PATH_LEN] = "";
    char staged_uninstall[MAX_PATH_LEN] = "";
    char staged_checksums[MAX_PATH_LEN] = "";
    char binary_url[MAX_MANIFEST_URL_LEN];
    char uninstall_url[MAX_MANIFEST_URL_LEN];
    char checksums_url[MAX_MANIFEST_URL_LEN];
    const char *platform_assets[2];
    int installed_exists;
    int binary_matches;
    int uninstall_matches;

    err = command_context_begin(&context, NULL, SYSTEM_LOCK_EXCLUSIVE);
    if (err != CUP_OK) {
        goto done;
    }
    err = command_require_no_transaction();
    if (err != CUP_OK) {
        goto done;
    }

    if (bootstrap_binary_asset_name(binary_name, sizeof(binary_name)) != CUP_OK ||
        bootstrap_platform_checksums_name(checksums_name,
            sizeof(checksums_name)) != CUP_OK ||
        layout_get_binary_path(installed_binary,
            sizeof(installed_binary)) != CUP_OK ||
        layout_get_uninstall_path(installed_uninstall,
            sizeof(installed_uninstall)) != CUP_OK ||
        layout_get_platform_checksums_path(installed_checksums,
            sizeof(installed_checksums)) != CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
        goto done;
    }

    err = system_is_regular_file(installed_binary, &installed_exists);
    if (err != CUP_OK) {
        goto done;
    }
    if (!installed_exists) {
        fprintf(stderr,
            "Error: no canonical cup executable is installed. Run the official "
            "installer before using self-update.\n");
        err = CUP_ERR_NOT_INSTALLED;
        goto done;
    }

    if (create_download_path("self-update-checksums", staged_checksums,
            sizeof(staged_checksums)) != CUP_OK ||
        create_download_path("self-update-binary", staged_binary,
            sizeof(staged_binary)) != CUP_OK ||
        create_download_path("self-update-uninstall", staged_uninstall,
            sizeof(staged_uninstall)) != CUP_OK ||
        text_format(checksums_url, sizeof(checksums_url), "%s/%s",
            CUP_BOOTSTRAP_URL, checksums_name) != CUP_OK ||
        text_format(binary_url, sizeof(binary_url), "%s/%s",
            CUP_BOOTSTRAP_URL, binary_name) != CUP_OK ||
        text_format(uninstall_url, sizeof(uninstall_url), "%s/%s",
            CUP_BOOTSTRAP_URL, CUP_UNINSTALL_FILENAME) != CUP_OK) {
        err = CUP_ERR_TEMPORARY;
        goto done;
    }

    printf("==> Checking for a cup update...\n");
    err = fetch_file(checksums_url, staged_checksums,
        FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }

    platform_assets[0] = binary_name;
    platform_assets[1] = CUP_UNINSTALL_FILENAME;
    err = checksum_validate_assets(staged_checksums, platform_assets,
        sizeof(platform_assets) / sizeof(platform_assets[0]));
    if (err != CUP_OK) {
        fprintf(stderr, "Error: remote platform checksums are invalid.\n");
        goto done;
    }

    err = installed_asset_matches(staged_checksums, binary_name,
        installed_binary, &binary_matches);
    if (err != CUP_OK) {
        goto done;
    }
    err = installed_asset_matches(staged_checksums, CUP_UNINSTALL_FILENAME,
        installed_uninstall, &uninstall_matches);
    if (err != CUP_OK) {
        goto done;
    }
    if (binary_matches && uninstall_matches) {
        printf("The canonical cup installation is already up to date.\n");
        err = CUP_OK;
        goto done;
    }

    printf("==> Downloading verified cup bootstrap assets...\n");
    err = fetch_file(binary_url, staged_binary, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }
    err = fetch_file(uninstall_url, staged_uninstall, FETCH_VALIDATE_NONEMPTY);
    if (err != CUP_OK) {
        goto done;
    }
    err = verify_downloaded_asset(staged_checksums, binary_name,
        staged_binary);
    if (err != CUP_OK) {
        goto done;
    }
    err = verify_downloaded_asset(staged_checksums, CUP_UNINSTALL_FILENAME,
        staged_uninstall);
    if (err != CUP_OK) {
        goto done;
    }

#if !defined(_WIN32)
    err = system_set_executable(staged_binary, 1);
    if (err == CUP_OK) {
        err = system_set_executable(staged_uninstall, 1);
    }
    if (err != CUP_OK) {
        goto done;
    }
#endif

    err = system_start_self_update(staged_binary, installed_binary,
        staged_uninstall, installed_uninstall,
        staged_checksums, installed_checksums, system_get_process_id());
    if (err != CUP_OK) {
        goto done;
    }

    staged_binary[0] = '\0';
    staged_uninstall[0] = '\0';
    staged_checksums[0] = '\0';
    printf("Verified cup update scheduled. The canonical bootstrap assets "
        "will be replaced after this process exits.\n");
    err = CUP_OK;

done:
    if (staged_checksums[0] != '\0') {
        system_remove_file(staged_checksums);
    }
    if (staged_uninstall[0] != '\0') {
        system_remove_file(staged_uninstall);
    }
    if (staged_binary[0] != '\0') {
        system_remove_file(staged_binary);
    }
    command_context_end(&context);
    return err;
}

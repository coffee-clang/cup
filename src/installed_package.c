/*
 * Checks package presence and validity across state and the canonical components tree. Command
 * modules add operation context.
 */

#include "installed_package.h"

#include "layout.h"
#include "package_selector.h"

#include <stdio.h>

/* State and filesystem presence are inspected together so callers never accept a half-present
 * package. */
static CupError get_presence(const CupState *state,
                             const PackageIdentity *package,
                             char *selector,
                             size_t selector_size,
                             int *in_state,
                             int *on_disk) {
    CupError err;

    if (state == NULL || package == NULL || selector == NULL || selector_size == 0 ||
        in_state == NULL || on_disk == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    err = package_identity_format_selector(package, selector, selector_size);
    if (err != CUP_OK) {
        return err;
    }

    *in_state = state_find_installed(state, package) != -1;
    return package_path_exists(package, on_disk);
}

CupError installed_package_require_present(const CupState *state, const PackageIdentity *package) {
    CupError err;
    char selector[MAX_SELECTOR_LEN];
    int in_state;
    int on_disk;

    err = get_presence(state, package, selector, sizeof(selector), &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }
    if (in_state && on_disk) {
        return CUP_OK;
    }
    if (!in_state && !on_disk) {
        fprintf(stderr,
                "Error: package '%s:%s' is not installed for host '%s', target '%s'.\n",
                package->component,
                selector,
                package->host_platform,
                package->target_platform);
        return CUP_ERR_NOT_INSTALLED;
    }

    fprintf(stderr,
            "Error: package '%s:%s' is inconsistent between state and components. "
            "Run 'cup doctor' and 'cup repair'.\n",
            package->component,
            selector);
    return CUP_ERR_INCONSISTENT_STATE;
}

/* A present package must also match its immutable metadata before an operation can reuse it. */
CupError installed_package_require_valid(const CupState *state, const PackageIdentity *package) {
    CupError err;
    char install_path[MAX_PATH_LEN];
    char selector[MAX_SELECTOR_LEN];

    err = installed_package_require_present(state, package);
    if (err != CUP_OK) {
        return err;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), package);
    if (err != CUP_OK) {
        return err;
    }
    err = package_validate(install_path, package);
    if (err == CUP_OK) {
        return CUP_OK;
    }
    if (err != CUP_ERR_VALIDATION) {
        return err;
    }

    if (package_identity_format_selector(package, selector, sizeof(selector)) != CUP_OK) {
        return CUP_ERR_INCONSISTENT_STATE;
    }
    fprintf(stderr,
            "Error: installed package '%s:%s' is invalid on disk. "
            "Run 'cup doctor' and 'cup repair'.\n",
            package->component,
            selector);
    return CUP_ERR_INCONSISTENT_STATE;
}

CupError installed_package_require_absent(const CupState *state, const PackageIdentity *package) {
    CupError err;
    char selector[MAX_SELECTOR_LEN];
    int in_state;
    int on_disk;

    err = get_presence(state, package, selector, sizeof(selector), &in_state, &on_disk);
    if (err != CUP_OK) {
        return err;
    }
    if (!in_state && !on_disk) {
        return CUP_OK;
    }
    if (in_state && on_disk) {
        return CUP_ERR_ALREADY_INSTALLED;
    }

    fprintf(stderr,
            "Error: package '%s:%s' is inconsistent between state and components. "
            "Run 'cup doctor' and 'cup repair'.\n",
            package->component,
            selector);
    return CUP_ERR_INCONSISTENT_STATE;
}

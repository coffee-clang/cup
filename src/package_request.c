/*
 * Validates command-level package requests and resolves symbolic releases through the package
 * catalog.
 */

#include "package_request.h"

#include "path.h"
#include "registry.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

/* Parse only CLI syntax here; catalog lookup is intentionally deferred to resolution. */
CupError package_request_parse(const char *component,
                               const char *selector_text,
                               PackageRequest *request) {
    CupError err;

    if (request == NULL || text_is_empty(component) || text_is_empty(selector_text)) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(request, 0, sizeof(*request));
    err = registry_validate_component(component);
    if (err != CUP_OK) {
        return err;
    }
    err = text_copy(request->input_selector, sizeof(request->input_selector), selector_text);
    if (err != CUP_OK) {
        return err;
    }
    err = package_selector_parse(&request->selector, selector_text);
    if (err != CUP_OK) {
        if (err == CUP_ERR_INVALID_RELEASE) {
            fprintf(stderr, "Error: invalid release identifier in '%s'.\n", selector_text);
            return err;
        }
        if (err == CUP_ERR_INVALID_TOOL) {
            fprintf(stderr, "Error: invalid tool identifier in '%s'.\n", selector_text);
            return err;
        }
        fprintf(stderr,
                "Error: invalid package selector '%s'. Expected '<tool>@<release>'.\n",
                selector_text);
        return CUP_ERR_INVALID_INPUT;
    }
    err = registry_validate_tool(component, request->selector.tool);
    if (err != CUP_OK) {
        return err;
    }
    if (!package_release_is_stable(request->selector.release) &&
        !path_is_safe_identifier(request->selector.release)) {
        fprintf(stderr, "Error: invalid release identifier '%s'.\n", request->selector.release);
        return CUP_ERR_INVALID_RELEASE;
    }
    return CUP_OK;
}

/* Resolve symbolic stable to one concrete release that is safe to persist. */
CupError package_request_resolve(const PackageCatalog *catalog,
                                 const char *component,
                                 const char *host_platform,
                                 const char *target_platform,
                                 PackageRequest *request) {
    CupError err;

    if (request == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (package_release_is_stable(request->selector.release)) {
        if (catalog == NULL) {
            return CUP_ERR_CATALOG;
        }
        err = package_catalog_resolve_stable(catalog,
                                             request->resolved_release,
                                             sizeof(request->resolved_release),
                                             component,
                                             request->selector.tool,
                                             host_platform,
                                             target_platform);
        if (err != CUP_OK) {
            return err;
        }
    } else {
        err = text_copy(request->resolved_release,
                        sizeof(request->resolved_release),
                        request->selector.release);
        if (err != CUP_OK) {
            return err;
        }
    }
    if (!path_is_safe_identifier(request->resolved_release)) {
        return CUP_ERR_INVALID_RELEASE;
    }
    return package_selector_format_parts(request->resolved_selector,
                                         sizeof(request->resolved_selector),
                                         request->selector.tool,
                                         request->resolved_release);
}

void package_request_print(FILE *stream, const PackageRequest *request) {
    if (stream == NULL || request == NULL) {
        return;
    }
    if (strcmp(request->input_selector, request->resolved_selector) == 0) {
        fprintf(stream, "%s", request->input_selector);
        return;
    }
    fprintf(stream, "%s -> %s", request->input_selector, request->resolved_selector);
}

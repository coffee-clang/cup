/*
 * Shows immutable metadata for one concrete installed package.
 */

#include "commands.h"
#include "installed_package.h"

#include "command_context.h"
#include "package_selector.h"
#include "package_request.h"
#include "wrappers.h"
#include "package_metadata.h"
#include "layout.h"
#include "package_catalog.h"
#include "package.h"
#include "path.h"
#include "registry.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

/* Package info.txt metadata output. */
static void print_metadata_field(const PackageMetadata *metadata,
                                 const char *key,
                                 const char *label) {
    const char *value = package_metadata_get(metadata, key);

    if (value != NULL) {
        printf("  %-18s %s\n", label, value);
    }
}

static void print_metadata_group(const PackageMetadata *metadata,
                                 const char *title,
                                 const char *prefix) {
    const PackageMetadataField *field;
    size_t cursor = 0;
    size_t prefix_length = strlen(prefix);
    int printed = 0;

    while ((field = package_metadata_next(metadata, prefix, &cursor)) != NULL) {
        if (!printed) {
            printf("\n%s:\n", title);
            printed = 1;
        }

        printf("  %-18s %s\n", field->key + prefix_length, field->value);
    }
}

static void print_package_commands(const PackageMetadata *metadata) {
    PackageCommand command;
    size_t cursor = 0;
    int printed = 0;

    while (package_metadata_next_command(metadata, &command, &cursor)) {
        if (!printed) {
            printf("\nCommands:\n");
            printed = 1;
        }
        printf("  %-18s %s\n", command.name, command.path);
    }
}

static void print_package_info(const PackageMetadata *metadata) {
    printf("Package:\n");
    print_metadata_field(metadata, "package.component", "component");
    print_metadata_field(metadata, "package.tool", "tool");
    print_metadata_field(metadata, "package.version", "version");
    print_metadata_field(metadata, "platform.host", "host");
    print_metadata_field(metadata, "platform.target", "target");
    print_package_commands(metadata);
    print_metadata_group(metadata, "Features", "features.");
    print_metadata_group(metadata, "Contents", "contents.");
    print_metadata_group(metadata, "Build/config", "config.");
}

CupError command_inspect(const char *component, const char *selector, const char *target_override) {
    CommandContext context = {0};
    PackageRequest request;
    PackageIdentity package;
    PackageMetadata metadata;
    CupError err;
    char install_path[MAX_PATH_LEN];
    char package_metadata_path[MAX_PATH_LEN];

    if (component == NULL || selector == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    /* Parse the public selector before opening any runtime resources. */
    package_metadata_init(&metadata);

    err = package_request_parse(component, selector, &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = command_context_begin_read_only(&context, target_override);
    if (err != CUP_OK) {
        goto done;
    }

    if (!context.runtime_available) {
        fprintf(stderr, "Error: package is not installed.\n");
        err = CUP_ERR_NOT_INSTALLED;
        goto done;
    }
    err = command_context_load_state(&context);
    if (err != CUP_OK) {
        goto done;
    }

    /* Stable selectors require the catalog; concrete versions can be resolved from state alone. */
    if (package_release_is_stable(request.selector.release)) {
        err = command_context_load_catalog(&context);
        if (err != CUP_OK) {
            goto done;
        }
    }

    err = package_request_resolve(context.has_catalog ? &context.catalog : NULL,
                                  component,
                                  context.host_platform,
                                  context.target_platform,
                                  &request);
    if (err != CUP_OK) {
        goto done;
    }

    err = package_identity_init(&package,
                                component,
                                request.selector.tool,
                                context.host_platform,
                                context.target_platform,
                                request.resolved_release);
    if (err != CUP_OK) {
        goto done;
    }

    /* The metadata is trusted only after state and the installed package agree. */
    err = installed_package_require_valid(&context.state, &package);
    if (err != CUP_OK) {
        goto done;
    }

    err = layout_build_install_path(install_path, sizeof(install_path), &package);
    if (err != CUP_OK || path_join(package_metadata_path,
                                   sizeof(package_metadata_path),
                                   install_path,
                                   CUP_INFO_FILENAME) != CUP_OK) {
        err = CUP_ERR_FILESYSTEM;
        goto done;
    }

    err = package_metadata_load(&metadata, package_metadata_path);
    if (err != CUP_OK) {
        goto done;
    }

    /* Print only after the complete metadata file has passed parsing and validation. */
    printf("Package information for %s ", component);
    package_request_print(stdout, &request);
    printf(" on host '%s', target '%s':\n\n", context.host_platform, context.target_platform);
    print_package_info(&metadata);
    err = CUP_OK;

done:
    package_metadata_free(&metadata);
    command_context_end(&context);
    return err;
}

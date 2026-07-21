#ifndef CUP_PACKAGE_CATALOG_H
#define CUP_PACKAGE_CATALOG_H

/*
 * Module contract: Strictly validated package catalog, stable-version
 * resolution, archive-format selection, and URL-template expansion.
 */

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Origin of the catalog currently loaded in memory. */
typedef enum {
    PACKAGE_CATALOG_SOURCE_NONE,
    PACKAGE_CATALOG_SOURCE_INSTALLED,
    PACKAGE_CATALOG_SOURCE_DEVELOPMENT
} PackageCatalogSource;

/* One complete component/tool/host/target package configuration. */
typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char stable_version[MAX_IDENTIFIER_LEN];
    char available_versions[MAX_CATALOG_VALUE_LEN];
    char default_format[MAX_IDENTIFIER_LEN];
    char formats[MAX_CATALOG_VALUE_LEN];
    char url_template[MAX_CATALOG_URL_LEN];
    char checksum_url_template[MAX_CATALOG_URL_LEN];
    unsigned field_mask;
} PackageCatalogEntry;

/* Dynamically sized catalog owned by the caller. */
typedef struct {
    PackageCatalogEntry *packages;
    size_t count;
    size_t capacity;
    PackageCatalogSource source;
    char path[MAX_PATH_LEN];
} PackageCatalog;

/* Initialize or release all storage owned by a PackageCatalog. */
void package_catalog_init(PackageCatalog *catalog);
void package_catalog_free(PackageCatalog *catalog);

/*
 * Load the active catalog. The repository copy is a development fallback
 * only when the installed catalog is absent, never when it is invalid.
 */
CupError package_catalog_load(PackageCatalog *catalog);

/* Load one explicitly selected source and validate the complete document. */
CupError package_catalog_load_installed(PackageCatalog *catalog);
CupError package_catalog_load_development(PackageCatalog *catalog);
CupError package_catalog_load_path(PackageCatalog *catalog,
                                   const char *path,
                                   PackageCatalogSource source);

/* Resolve stable or query one concrete version in an exact package tuple. */
CupError package_catalog_resolve_stable(const PackageCatalog *catalog,
                                        char *buffer,
                                        size_t size,
                                        const char *component,
                                        const char *tool,
                                        const char *host_platform,
                                        const char *target_platform);
CupError package_catalog_is_stable(const PackageCatalog *catalog,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   int *is_stable);
CupError package_catalog_has_package(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     int *is_available);
CupError package_catalog_has_version(const PackageCatalog *catalog,
                                     const char *component,
                                     const char *tool,
                                     const char *host_platform,
                                     const char *target_platform,
                                     const char *version,
                                     int *is_available);

/* Resolve the default format or query one supported format in a tuple. */
CupError package_catalog_get_default_format(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform);
CupError package_catalog_has_format(const PackageCatalog *catalog,
                                    const char *component,
                                    const char *tool,
                                    const char *host_platform,
                                    const char *target_platform,
                                    const char *format,
                                    int *is_supported);

/* Expand validated HTTPS templates for one concrete package identity. */
CupError package_catalog_build_url(const PackageCatalog *catalog,
                                   char *buffer,
                                   size_t size,
                                   const char *component,
                                   const char *tool,
                                   const char *host_platform,
                                   const char *target_platform,
                                   const char *version,
                                   const char *format);
CupError package_catalog_build_checksum_url(const PackageCatalog *catalog,
                                            char *buffer,
                                            size_t size,
                                            const char *component,
                                            const char *tool,
                                            const char *host_platform,
                                            const char *target_platform,
                                            const char *version);

#endif /* CUP_PACKAGE_CATALOG_H */

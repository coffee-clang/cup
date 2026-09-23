#ifndef CUP_PACKAGE_CATALOG_H
#define CUP_PACKAGE_CATALOG_H

/*
 * Concrete package availability loaded from catalog.cfg. Structurally valid future records may
 * be retained but are ignored by runtime queries until this cup generation owns their contract.
 */

#include <stddef.h>
#include <stdint.h>

#include "checksum.h"
#include "constants.h"
#include "error.h"
#include "system.h"

typedef struct {
    char format[MAX_IDENTIFIER_LEN];
    char url[MAX_CATALOG_URL_LEN];
    char sha256[CHECKSUM_SHA256_HEX_LENGTH + 1];
    unsigned field_mask;
} PackageCatalogArtifact;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char version[MAX_IDENTIFIER_LEN];
    char revision_reason[MAX_CATALOG_VALUE_LEN];
    int stable;
    int operational;
    unsigned field_mask;
    PackageCatalogArtifact *artifacts;
    size_t artifact_count;
    size_t artifact_capacity;
} PackageCatalogEntry;

typedef struct {
    PackageCatalogEntry *packages;
    size_t count;
    size_t capacity;
    uint64_t revision;
    char update_url[MAX_CATALOG_URL_LEN];
    SystemPathIdentity identity;
    char digest[CHECKSUM_SHA256_HEX_LENGTH + 1];
} PackageCatalog;

void package_catalog_init(PackageCatalog *catalog);
void package_catalog_free(PackageCatalog *catalog);

CupError package_catalog_load(PackageCatalog *catalog);
CupError package_catalog_load_installed(PackageCatalog *catalog);
CupError package_catalog_load_development(PackageCatalog *catalog);
/* Copy the local development catalog into a missing runtime catalog. */
CupError package_catalog_seed_runtime(void);
CupError package_catalog_load_path(PackageCatalog *catalog, const char *path);

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

CupError package_catalog_resolve_artifact(const PackageCatalog *catalog,
                                          const char *component,
                                          const char *tool,
                                          const char *host_platform,
                                          const char *target_platform,
                                          const char *version,
                                          const char *format,
                                          char *url,
                                          size_t url_size,
                                          char *artifact_sha256,
                                          size_t artifact_sha256_size);

#endif /* CUP_PACKAGE_CATALOG_H */

#ifndef CUP_MANIFEST_H
#define CUP_MANIFEST_H

#include <stddef.h>

#include "constants.h"
#include "error.h"

/* Origin of the manifest currently loaded in memory. */
typedef enum {
    MANIFEST_SOURCE_NONE,
    MANIFEST_SOURCE_INSTALLED,
    MANIFEST_SOURCE_DEVELOPMENT
} ManifestSource;

/* One component/tool/host/target package configuration. */
typedef struct {
    char component[MAX_NAME_LEN];
    char tool[MAX_NAME_LEN];
    char host_platform[MAX_PLATFORM_LEN];
    char target_platform[MAX_PLATFORM_LEN];
    char stable_version[MAX_NAME_LEN];
    char available_versions[MAX_MANIFEST_VALUE_LEN];
    char default_format[MAX_NAME_LEN];
    char formats[MAX_MANIFEST_VALUE_LEN];
    char url_template[MAX_MANIFEST_URL_LEN];
    char checksum_url_template[MAX_MANIFEST_URL_LEN];
    unsigned field_mask;
} ManifestPackage;

/* Dynamically sized package manifest. */
typedef struct {
    ManifestPackage *packages;
    size_t count;
    size_t capacity;
    ManifestSource source;
    char path[MAX_PATH_LEN];
} Manifest;

/* Initialize or release a Manifest object. */
void manifest_init(Manifest *manifest);
void manifest_free(Manifest *manifest);

/* Load the active manifest, using the repository fallback only when the
 * installed file is missing. */
CupError manifest_load(Manifest *manifest);

/* Load only the installed manifest or an explicitly selected file. */
CupError manifest_load_installed(Manifest *manifest);
CupError manifest_load_development(Manifest *manifest);
CupError manifest_load_path(Manifest *manifest, const char *path, ManifestSource source);

/* Resolve and query package versions. */
CupError manifest_resolve_stable(const Manifest *manifest, char *buffer, size_t size,
    const char *component, const char *tool, const char *host_platform,
    const char *target_platform);
CupError manifest_is_stable(const Manifest *manifest, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *version,
    int *is_stable);
CupError manifest_has_version(const Manifest *manifest, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *version,
    int *is_available);

/* Resolve and query supported archive formats. */
CupError manifest_get_default_format(const Manifest *manifest, char *buffer, size_t size,
    const char *component, const char *tool, const char *host_platform,
    const char *target_platform);
CupError manifest_has_format(const Manifest *manifest, const char *component, const char *tool,
    const char *host_platform, const char *target_platform, const char *format,
    int *is_supported);

/* Expand the URL template for one concrete package. */
CupError manifest_build_url(const Manifest *manifest, char *buffer, size_t size,
    const char *component, const char *tool, const char *host_platform,
    const char *target_platform, const char *version, const char *format);
CupError manifest_build_checksum_url(const Manifest *manifest, char *buffer,
    size_t size, const char *component, const char *tool,
    const char *host_platform, const char *target_platform,
    const char *version);

#endif /* CUP_MANIFEST_H */

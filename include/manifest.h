#ifndef CUP_MANIFEST_H
#define CUP_MANIFEST_H

#include <stddef.h>

#include "error.h"
#include "constants.h"

typedef struct {
    char key[MAX_MANIFEST_KEY_LEN];
    char value[MAX_MANIFEST_VALUE_LEN];
} ManifestField;

typedef struct {
    ManifestField fields[MAX_MANIFEST_FIELDS];
    size_t count;
} Manifest;

// MANIFEST DATA
CupError manifest_load(Manifest *manifest);
const char *get_manifest_value(const Manifest *manifest, const char *key);

// RELEASE
CupError resolve_stable_release(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform);
CupError is_stable_version(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_stable);
CupError is_version_available(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_available);

// FORMAT
CupError get_default_format(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform);
CupError is_format_supported(const Manifest *manifest, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *format, int *is_supported);

// URL
CupError build_download_url(const Manifest *manifest, char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, const char *format);

#endif /* CUP_MANIFEST_H */
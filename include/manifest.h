#ifndef CUP_MANIFEST_H
#define CUP_MANIFEST_H

#include <stddef.h>

#include "error.h"

// RELEASE
CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *platform, const char *release);
CupError is_stable_release(const char *component, const char *tool, const char *platform, const char *release, int *is_stable);
CupError is_version_available(const char *component, const char *tool, const char *platform, const char *version, int *is_available);

// FORMAT
CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool, const char *platform);
CupError is_format_supported(const char *component, const char *tool, const char *platform, const char *format, int *is_supported);

// URL
CupError build_package_url_from_manifest(char *buffer, size_t size, const char *component, const char *tool, const char *platform, const char *release, const char *format);

#endif /* CUP_MANIFEST_H */
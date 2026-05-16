#ifndef CUP_MANIFEST_H
#define CUP_MANIFEST_H

#include <stddef.h>

#include "error.h"

// RELEASE
CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *release);
CupError is_stable_version(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_stable);
CupError is_version_available(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *is_available);

// FORMAT
CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform);
CupError is_format_supported(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *format, int *is_supported);

// URL
CupError build_download_url(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, const char *format);

#endif /* CUP_MANIFEST_H */
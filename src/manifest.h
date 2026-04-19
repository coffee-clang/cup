#ifndef MANIFEST_H
#define MANIFEST_H

#include <stddef.h>

#include "state.h"
#include "error.h"

// PATH
CupError get_package_manifest_path(char *buffer, size_t size);

// LOOKUP
CupError read_manifest_value(char *buffer, size_t size, const char *component, const char *tool, const char *key_suffix);

// RELEASE
CupError resolve_release(char *buffer, size_t size, const char *component, const char *tool, const char *release);

// FORMAT
CupError get_default_format(char *buffer, size_t size, const char *component, const char *tool);
CupError is_format_supported(const char *component, const char *tool, const char *format, int *is_supported);

// URL
CupError build_package_url_from_manifest(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *format);

#endif
/*
 * Parses the closed public package archive domain.
 */

#include "package_archive.h"

#include "text.h"

#include <string.h>

/* Closed archive-format domain shared by the catalog, cache and extractor. */
CupError package_archive_parse_format(const char *value, PackageArchiveFormat *format) {
    if (format == NULL || text_is_empty(value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(value, "tar.xz") == 0) {
        *format = PACKAGE_ARCHIVE_FORMAT_TAR_XZ;
    } else if (strcmp(value, "tar.gz") == 0) {
        *format = PACKAGE_ARCHIVE_FORMAT_TAR_GZ;
    } else if (strcmp(value, "zip") == 0) {
        *format = PACKAGE_ARCHIVE_FORMAT_ZIP;
    } else {
        return CUP_ERR_VALIDATION;
    }

    return CUP_OK;
}

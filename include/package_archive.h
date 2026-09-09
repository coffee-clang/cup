#ifndef CUP_PACKAGE_ARCHIVE_H
#define CUP_PACKAGE_ARCHIVE_H

/*
 * Closed package-archive formats. cup accepts only tar.xz, tar.gz, and zip and validates
 * libarchive's detected format/filter stack during the actual extraction pass.
 */

#include <stdio.h>

#include "error.h"

struct archive;

typedef enum {
    PACKAGE_ARCHIVE_FORMAT_ANY,
    PACKAGE_ARCHIVE_FORMAT_TAR_XZ,
    PACKAGE_ARCHIVE_FORMAT_TAR_GZ,
    PACKAGE_ARCHIVE_FORMAT_ZIP
} PackageArchiveFormat;

CupError package_archive_parse_format(const char *value, PackageArchiveFormat *format);
const char *package_archive_format_name(PackageArchiveFormat format);

/* Open one already owned stream with only cup-supported formats and filters enabled. */
CupError package_archive_open_stream(struct archive **reader, FILE *file);

/* Compare the detected format/filter stack after the first header was read. */
int package_archive_reader_matches_format(struct archive *reader,
                                          PackageArchiveFormat expected_format);

#endif /* CUP_PACKAGE_ARCHIVE_H */

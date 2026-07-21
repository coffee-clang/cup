#ifndef CUP_PACKAGE_ARCHIVE_H
#define CUP_PACKAGE_ARCHIVE_H

/*
 * Module contract: Closed package-archive formats and bounded structural
 * preflight. CUP accepts only tar.xz, tar.gz and zip, and validates the real
 * libarchive format/filter stack against the catalog selection.
 */

#include "error.h"

struct archive;

typedef enum {
    PACKAGE_ARCHIVE_FORMAT_ANY,
    PACKAGE_ARCHIVE_FORMAT_TAR_XZ,
    PACKAGE_ARCHIVE_FORMAT_TAR_GZ,
    PACKAGE_ARCHIVE_FORMAT_ZIP
} PackageArchiveFormat;

/* Parse one public archive-format identifier from the closed CUP domain. */
CupError package_archive_parse_format(const char *value, PackageArchiveFormat *format);

/* Open a reader configured only for CUP-supported formats and filters. */
CupError package_archive_open_reader(struct archive **reader, const char *archive_path);

/* Compare the detected format/filter stack after the first header was read. */
int package_archive_reader_matches_format(struct archive *reader,
                                          PackageArchiveFormat expected_format);

/* Read the complete archive and report structural and real-format validity. */
CupError package_archive_is_valid(const char *archive_path,
                                  const char *expected_format,
                                  int *is_valid);

#endif /* CUP_PACKAGE_ARCHIVE_H */

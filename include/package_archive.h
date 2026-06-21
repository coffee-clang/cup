#ifndef CUP_PACKAGE_ARCHIVE_H
#define CUP_PACKAGE_ARCHIVE_H

#include "error.h"

struct archive;

/* Open an archive with every format and filter supported by libarchive. */
CupError package_archive_open_reader(struct archive **reader,
    const char *archive_path);

/* Read the complete archive and report whether it is structurally valid. */
CupError package_archive_is_valid(const char *archive_path, int *is_valid);

#endif /* CUP_PACKAGE_ARCHIVE_H */

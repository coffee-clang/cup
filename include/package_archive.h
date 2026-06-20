#ifndef CUP_PACKAGE_ARCHIVE_H
#define CUP_PACKAGE_ARCHIVE_H

#include "error.h"

struct archive;

/* Open an archive with the formats supported by cup. */
CupError package_archive_open_reader(struct archive **reader, const char *archive_path);

/* Perform a lightweight check before reusing a cached archive. */
CupError package_archive_is_usable(const char *archive_path, int *is_usable);

#endif /* CUP_PACKAGE_ARCHIVE_H */

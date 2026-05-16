#ifndef CUP_PACKAGE_ARCHIVE_H
#define CUP_PACKAGE_ARCHIVE_H

#include "error.h"

struct archive;

CupError package_archive_open_reader(struct archive **reader, const char *archive_path);
CupError package_archive_is_usable(const char *archive_path, int *is_usable);

#endif /* CUP_PACKAGE_ARCHIVE_H */
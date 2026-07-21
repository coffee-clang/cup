#ifndef CUP_PACKAGE_EXTRACT_H
#define CUP_PACKAGE_EXTRACT_H

/*
 * Module contract: Safe extraction of one supported package archive into a
 * caller-provided fresh staging directory. Paths are validated for every
 * supported filesystem before any entry can escape or alias another entry.
 */

#include "error.h"

CupError package_extract_archive(const char *archive_path,
                                 const char *staging_path,
                                 const char *format);

#endif /* CUP_PACKAGE_EXTRACT_H */

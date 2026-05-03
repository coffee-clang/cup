#ifndef CUP_EXTRACT_H
#define CUP_EXTRACT_H

#include "error.h"

CupError extract_archive(const char *archive_path, const char *tmp_path);

#endif /* CUP_EXTRACT_H */
#ifndef ARCHIVE_H
#define ARCHIVE_H

#include "error.h"

CupError extract_archive_to_tmp(const char *archive_path, const char *tmp_path);

#endif
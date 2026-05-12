#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

#include <stddef.h>

#include "error.h"

// STATE PATH
CupError get_state_file_path(char *buffer, size_t size);

// STRUCTURE
CupError ensure_cup_structure(void);
CupError ensure_component_base_dirs(const char *component, const char *tool, const char *host_platform, const char *target_platform);
CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *version);

// PATH BUILDERS
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, 
    const char *target_platform, const char *version);
CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *version, 
    const char *archive_name);

// INSTALL FLOW
CupError create_tmp_dir(char *buffer, size_t size, const char *operation, const char *component, const char *tool, 
    const char *version);
CupError validate_install(const char *tmp_path);
CupError commit_path(const char *source_path, const char *destination_path);

// CLEANUP
CupError cleanup_tmp_path(const char *tmp_path);
CupError cleanup_all_tmp(void);

// CONSISTENCY
CupError installation_exists(const char *component, const char *tool, const char *host_platform, const char *target_platform, 
    const char *version, int *exists);
CupError archive_exists(const char *archive_path, int *exists);
CupError archive_is_usable(const char *archive_path, int *is_usable);

#endif /* CUP_FILESYSTEM_H */
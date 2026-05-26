#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

#include <stddef.h>

#include "error.h"

// STRUCTURE
CupError check_cup_structure(size_t *missing_count);
CupError ensure_cup_structure(void);
CupError ensure_component_base_dirs(const char *component, const char *tool, const char *host_platform, const char *target_platform);
CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *version);

// PATHS
CupError get_cup_root_path(char *buffer, size_t size);
CupError get_state_file_path(char *buffer, size_t size);
CupError get_manifest_file_path(char *buffer, size_t size);
CupError get_uninstall_script_path(char *buffer, size_t size);
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version);
CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *version, const char *archive_name);

// FILE OPERATIONS
CupError copy_file(const char *source_path, const char *destination_path);

// INSTALL
CupError create_tmp_dir(char *buffer, size_t size, const char *operation, const char *component, const char *tool, const char *version);
CupError validate_installation_metadata(const char *tmp_path, const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version);

// CLEANUP
CupError count_tmp_entries(size_t *count);
CupError cleanup_tmp_path(const char *tmp_path);
CupError cleanup_all_tmp(void);

// INSTALL QUERY
CupError install_dir_exists(const char *component, const char *tool, const char *host_platform, const char *target_platform, const char *version, int *exists);

#endif /* CUP_FILESYSTEM_H */
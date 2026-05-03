#ifndef CUP_FILESYSTEM_H
#define CUP_FILESYSTEM_H

#include <stddef.h>

#include "error.h"

// STATE PATH
CupError get_state_file_path(char *buffer, size_t size);

// STRUCTURE
CupError ensure_cup_structure(void);
CupError ensure_component_base_dirs(const char *component, const char *tool, const char *platform);
CupError ensure_cache_package_dirs(const char *component, const char *tool, const char *release);

// PATH BUILDERS
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *platform, const char *release);
CupError build_cache_archive_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, const char *platform, const char *archive_format);

// INSTALL FLOW
CupError create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release);
CupError create_tmp_remove_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release);
CupError validate_install(const char *tmp_path);
CupError commit_path(const char *source_path, const char *destination_path);

// CLEANUP
CupError cleanup_tmp_install(const char *tmp_path);
CupError cleanup_all_tmp(void);

// CONSISTENCY
CupError installation_exists(const char *component, const char *tool, const char *platform, const char *release, int *exists);
CupError archive_exists(const char *archive_path, int *exists);
CupError archive_is_usable(const char *archive_path, int *is_usable);

// METADATA
CupError write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *platform, const char *release);

#endif /* CUP_FILESYSTEM_H */
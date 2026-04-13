#ifndef FS_H
#define FS_H

#include <stddef.h>

#include "state.h"
#include "error.h"

// UTILS
CupError checked_snprintf(char *buffer, size_t size, const char *format, ...);

// BASE PATH
const char *get_platform_name(void);

CupError get_cup_root_path(char *buffer, size_t size);
CupError get_tmp_root_path(char *buffer, size_t size);
CupError get_components_root_path(char *buffer, size_t size);
CupError get_state_file_path(char *buffer, size_t size);

// STRUCTURE
CupError ensure_cup_structure(void);
CupError ensure_component_dirs(const char *component, const char *tool, const char *platform);

// PATH BUILDERS
CupError build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release);
CupError build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, int suffix);

// INSTALL FLOW
CupError create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release);

CupError simulate_install(const char *tmp_path, const char *component, const char *tool, const char *release);

CupError validate_install(const char *tmp_path);
CupError commit_install(const char *tmp_path, const char *final_path);
CupError cleanup_tmp_install(const char *tmp_path);
CupError cleanup_all_tmp(void);

// METADATA
CupError write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *release);

// CONSISTENCY
CupError installation_exists(const char *component, const char *tool, const char *release, int *exists);

// REMOVE
CupError remove_component_install_dir(const char *component, const char *tool, const char *release);

#endif
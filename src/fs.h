#ifndef FS_H
#define FS_H

#include <stddef.h>
#include "state.h"

// UTILS
int checked_snprintf(char *buffer, size_t size, const char *format, ...);

// BASE PATH
const char *get_platform_name(void);

int get_cup_root_path(char *buffer, size_t size);
int get_tmp_root_path(char *buffer, size_t size);
int get_components_root_path(char *buffer, size_t size);
int get_state_file_path(char *buffer, size_t size);

// STRUCTURE
int ensure_cup_structure(void);
int ensure_component_dirs(const char *component, const char *tool, const char *platform);

// PATH BUILDERS
int build_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release);
int build_tmp_install_path(char *buffer, size_t size, const char *component, const char *tool, const char *release, int suffix);

// INSTALL FLOW
int create_tmp_install_dir(char *buffer, size_t size, const char *component, const char *tool, const char *release);

int simulate_install(const char *tmp_path, const char *component, const char *tool, const char *release);

int validate_install(const char *tmp_path);
int commit_install(const char *tmp_path, const char *final_path);
int cleanup_tmp_install(const char *tmp_path);

// METADATA
int write_component_info_at_path(const char *base_path, const char *component, const char *tool, const char *release);

// REMOVE
int remove_component_install_dir(const char *component, const char *tool, const char *release);

#endif
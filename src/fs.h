#ifndef FS_H
#define FS_H

#include <stddef.h>

#include "state.h"

int checked_snprintf(char *buffer, size_t size, const char *format, ...);

const char *get_platform_name(void);
int get_cup_root_path(char *buffer, size_t size);
int get_state_file_path(char *buffer, size_t size);

int ensure_cup_structure(void);

int create_component_installation_dir(const char *component, const char *tool, const char *release);
int remove_component_installation_dir(const char *component, const char *tool, const char *release);
int write_component_info(const char *component, const char *tool, const char *release);

#endif
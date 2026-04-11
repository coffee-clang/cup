#ifndef COMPONENT_H
#define COMPONENT_H

int handle_list(void);
int handle_install(const char *component, const char *entry);
int handle_remove(const char *component, const char *entry);
int handle_default(const char *component, const char *entry);
int handle_current(const char *component);

#endif
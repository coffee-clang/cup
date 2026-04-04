#ifndef TOOLCHAiN_H
#define TOOLCHAIN_H

int handle_list(void);
int handle_install(const char *name);
int handle_remove(const char *name);
int handle_default(const char *name);
int handle_current(void);

#endif
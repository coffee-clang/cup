#ifndef CUP_OPTIONS_H
#define CUP_OPTIONS_H

#include "error.h"

typedef struct {
    const char *format;
    const char *platform;
    unsigned seen;
} CommandOptions;

#define OPT_FORMAT (1u << 0)
#define OPT_PLATFORM (1u << 1)

CupError parse_command_options(int start_option, int argc, char *argv[], CommandOptions *options);

CupError validate_command_options(const CommandOptions *options, unsigned allowed_options, const char *command_name);

#endif /* CUP_OPTIONS_H */
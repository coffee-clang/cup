#ifndef CUP_OPTIONS_H
#define CUP_OPTIONS_H

#include "error.h"

#define OPT_FORMAT (1u << 0)
#define OPT_TARGET (1u << 1)

typedef unsigned OptionFlags;

typedef struct {
    const char *format;
    const char *target;
    OptionFlags seen;
} CommandOptions;

CupError parse_command_options(int start_option, int argc, char *const argv[], CommandOptions *options);
CupError validate_command_options(const CommandOptions *options, OptionFlags allowed_options, const char *command_name);

#endif /* CUP_OPTIONS_H */
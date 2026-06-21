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

CupError options_parse(int first_option, int argc, char *const argv[],
    CommandOptions *options);
CupError options_validate(const CommandOptions *options,
    OptionFlags allowed, const char *command_name);

#endif /* CUP_OPTIONS_H */

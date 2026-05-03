#include "options.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    OPTION_VALUE_NONE = 0,
    OPTION_VALUE_REQUIRED
} OptionValueMode;

typedef enum {
    OPTION_ID_FORMAT = 0,
    OPTION_ID_PLATFORM
} OptionId;

typedef struct {
    const char *long_name;
    const char *short_name;
    OptionId id;
    unsigned flag;
    OptionValueMode value_mode;
} OptionDefinition;

static const OptionDefinition OPTION_DEFINITIONS[] = {
    {
        "--format",
        "-f",
        OPTION_ID_FORMAT,
        OPT_FORMAT,
        OPTION_VALUE_REQUIRED
    },
    {
        "--platform",
        NULL,
        OPTION_ID_PLATFORM,
        OPT_PLATFORM,
        OPTION_VALUE_REQUIRED
    }
};

static void init_command_options(CommandOptions *options) {
    if (options == NULL) {
        return;
    }

    options->format = NULL;
    options->platform = NULL;
    options->seen = 0;
}

static const OptionDefinition *find_option_definition(const char *name) {
    size_t count;
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    count = sizeof(OPTION_DEFINITIONS) / sizeof(OPTION_DEFINITIONS[0]);

    for (i = 0; i < count; ++i) {
        if (strcmp(name, OPTION_DEFINITIONS[i].long_name) == 0) {
            return &OPTION_DEFINITIONS[i];
        }

        if (OPTION_DEFINITIONS[i].short_name != NULL &&
            strcmp(name, OPTION_DEFINITIONS[i].short_name) == 0) {
            return &OPTION_DEFINITIONS[i];
        }
    }

    return NULL;
}

static CupError set_option_value(CommandOptions *options, const OptionDefinition *definition, const char *value) {
    if (options == NULL || definition == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    
    if ((options->seen & definition->flag) != 0) {
        fprintf(stderr, "Error: duplicate option '%s'.\n", definition->long_name);
        return CUP_ERR_INVALID_INPUT;
    }

    options->seen |= definition->flag;

    switch (definition->id) {
        case OPTION_ID_FORMAT:
            options->format = value;
            return CUP_OK;

        case OPTION_ID_PLATFORM:
            options->platform = value;
            return CUP_OK;

        default:
            return CUP_ERR_INVALID_INPUT;
    }
}

CupError parse_command_options(int start_option, int argc, char *argv[], CommandOptions *options) {
    CupError err;
    int i;

    if (argv == NULL || options == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    init_command_options(options);

    if ((argc - start_option) % 2 != 0) {
        fprintf(stderr, "Error: command options must be passed as '--option value'.\n");
    }

    i = start_option;
    while (i < argc) {
        const char *name;
        const char *value;
        const OptionDefinition *definition;

        name = argv[i];
        definition = find_option_definition(name);
        if (definition == NULL) {
            fprintf(stderr, "Error: unknown option '%s'.\n", name);
            return CUP_ERR_INVALID_INPUT;
        }

        if (definition->value_mode == OPTION_VALUE_REQUIRED) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: missing value for option '%s'.\n", definition->long_name);
                return CUP_ERR_INVALID_INPUT;
            }

            value = argv[i + 1];

            if (value[0] == '-') {
                fprintf(stderr, "Error: missing value for option '%s'.\n", definition->long_name);
                return CUP_ERR_INVALID_INPUT;
            }

            err = set_option_value(options, definition, value);
            if (err != CUP_OK) {
                return CUP_ERR_INVALID_INPUT;
            }

            i += 2;
            continue;
        }

        err = set_option_value(options, definition, NULL);
        if (err != CUP_OK) {
            return CUP_ERR_INVALID_INPUT;
        }

        i += 1;
    }

    return CUP_OK;
}

CupError validate_command_options(const CommandOptions *options, unsigned allowed_options) {
    size_t count;
    size_t i;
    unsigned disallowed;

    if (options == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    disallowed = options->seen & ~allowed_options;
    if (disallowed == 0) {
        return CUP_OK;
    }

    count = sizeof(OPTION_DEFINITIONS) / sizeof(OPTION_DEFINITIONS[0]);

    for (i = 0; i < count; ++i) {
        if ((disallowed & OPTION_DEFINITIONS[i].flag) != 0) {
            fprintf(stderr, "Error: option '%s' is not valid for this command.\n", OPTION_DEFINITIONS[i].long_name);
            return CUP_ERR_INVALID_INPUT;
        }
    }

    return CUP_ERR_INVALID_INPUT;
}
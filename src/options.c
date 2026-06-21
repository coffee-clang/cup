#include "options.h"

#include "text.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    OPTION_FORMAT,
    OPTION_TARGET
} OptionId;

typedef struct {
    const char *long_name;
    const char *short_name;
    OptionId id;
    OptionFlags flag;
} OptionDefinition;

static const OptionDefinition OPTION_DEFINITIONS[] = {
    { "--format", "-f", OPTION_FORMAT, OPT_FORMAT },
    { "--target", NULL, OPTION_TARGET, OPT_TARGET }
};

static const OptionDefinition *find_option(const char *name) {
    size_t i;

    if (text_is_empty(name)) {
        return NULL;
    }

    for (i = 0; i < sizeof(OPTION_DEFINITIONS) / sizeof(OPTION_DEFINITIONS[0]); ++i) {
        const OptionDefinition *option = &OPTION_DEFINITIONS[i];

        if (strcmp(name, option->long_name) == 0 ||
            (option->short_name != NULL && strcmp(name, option->short_name) == 0)) {
            return option;
        }
    }

    return NULL;
}

static CupError assign_option(CommandOptions *options,
    const OptionDefinition *definition, const char *value) {
    if (options == NULL || definition == NULL || text_is_empty(value)) {
        return CUP_ERR_INVALID_INPUT;
    }

    if ((options->seen & definition->flag) != 0) {
        fprintf(stderr, "Error: duplicate option '%s'.\n", definition->long_name);
        return CUP_ERR_INVALID_INPUT;
    }

    switch (definition->id) {
        case OPTION_FORMAT:
            options->format = value;
            break;

        case OPTION_TARGET:
            options->target = value;
            break;

        default:
            return CUP_ERR_INVALID_INPUT;
    }

    options->seen |= definition->flag;
    return CUP_OK;
}

CupError options_parse(int first_option, int argc, char *const argv[],
    CommandOptions *options) {
    int index;

    if (argc < 0 || first_option < 0 || first_option > argc ||
        argv == NULL || options == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    memset(options, 0, sizeof(*options));

    for (index = first_option; index < argc; index += 2) {
        const OptionDefinition *definition;
        const char *name = argv[index];
        const char *value;

        definition = find_option(name);
        if (definition == NULL) {
            fprintf(stderr, "Error: unknown option '%s'.\n", name);
            return CUP_ERR_INVALID_INPUT;
        }

        if (index + 1 >= argc || find_option(argv[index + 1]) != NULL) {
            fprintf(stderr, "Error: missing value for option '%s'.\n",
                definition->long_name);
            return CUP_ERR_INVALID_INPUT;
        }

        value = argv[index + 1];
        if (assign_option(options, definition, value) != CUP_OK) {
            return CUP_ERR_INVALID_INPUT;
        }
    }

    return CUP_OK;
}

CupError options_validate(const CommandOptions *options,
    OptionFlags allowed, const char *command_name) {
    OptionFlags disallowed;
    size_t i;

    if (options == NULL || text_is_empty(command_name)) {
        return CUP_ERR_INVALID_INPUT;
    }

    disallowed = options->seen & ~allowed;
    if (disallowed == 0) {
        return CUP_OK;
    }

    for (i = 0; i < sizeof(OPTION_DEFINITIONS) / sizeof(OPTION_DEFINITIONS[0]); ++i) {
        if ((disallowed & OPTION_DEFINITIONS[i].flag) != 0) {
            fprintf(stderr, "Error: option '%s' is not valid for command '%s'.\n",
                OPTION_DEFINITIONS[i].long_name, command_name);
            return CUP_ERR_INVALID_INPUT;
        }
    }

    return CUP_ERR_INVALID_INPUT;
}

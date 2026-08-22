/*
 * Defines command help, Argtable3 argument schemas and the top-level command dispatcher. Runtime
 * behavior remains in command modules.
 */

#include "commands.h"

#include "error.h"
#include "exit_status.h"
#include "interrupt.h"
#include "layout.h"
#include "update_helper.h"
#include "bootstrap.h"
#include "uninstall_helper.h"
#include "system.h"
#include "package_selector.h"
#include "package_archive.h"
#include "path.h"
#include "platform.h"
#include "registry.h"
#include "text.h"
#include "version.h"

#include "argtable3.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *details;
} CommandHelp;

/* Command metadata and help rendering. */
static const CommandHelp COMMAND_HELP[] = {
    {"help",
     "help [command]",
     "Show general or command-specific help.",
     "Description:\n  Show the command list or detailed help for one command.\n"
     "Arguments:\n  command  Optional public command name.\n"
     "Options:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Without command, show general help.\n"
     "Examples:\n  cup help\n  cup help install\n"
     "Effects:\n  Read-only; never initializes the local cup installation."},
    {"search",
     "search [<component>] [--target <target-platform>]",
     "Search the package catalog.",
     "Description:\n  Show packages available from the current catalog.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict results to one target.\n"
     "  -h, --help                   Show this help.\n"
     "Defaults:\n  Uses the current host and shows every target when --target is omitted.\n"
     "Examples:\n  cup search\n  cup search compiler --target linux-x64\n"
     "Effects:\n  Read-only; never initializes the local cup installation."},
    {"list",
     "list [<component>] [--target <target-platform>]",
     "List installed packages.",
     "Description:\n  Show installed package versions with default and stable annotations.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict results to one target.\n"
     "  -h, --help                   Show this help.\n"
     "Defaults:\n  Uses the current host and all installed targets.\n"
     "Examples:\n  cup list\n  cup list compiler --target linux-x64\n"
     "Effects:\n  Read-only; invalid package entries are reported and return a nonzero status."},
    {"install",
     "install [<component>] <tool>[@<release>] [--target <target-platform>] "
     "[--format|-f <archive-format>] | install <component> [--target <target-platform>] "
     "[--format|-f <archive-format>] | install <profile|toolchain> <name> "
     "[--target <target-platform>] [--format|-f <archive-format>]",
     "Install one package, profile or toolchain.",
     "Description:\n  Resolve and install one package, profile or toolchain.\n"
     "Arguments:\n  component          Component to install or explicit component for a tool.\n"
     "  tool[@release]     Tool selection; release defaults to stable.\n"
     "  profile|toolchain  Group kind followed by its name.\n"
     "Options:\n  --target <target-platform>  Select the target platform.\n"
     "  -f, --format <format>       Select tar.xz, tar.gz or zip.\n"
     "  -h, --help                  Show this help.\n"
     "Defaults:\n  An omitted tool uses the preferred tool, then the official default.\n"
     "  An omitted release means stable.\n"
     "Examples:\n  cup install gcc\n  cup install gcc@stable\n  cup install compiler\n"
     "  cup install compiler gcc@stable\n  cup install profile "
     "standard\n  cup install toolchain llvm\n"
     "Effects:\n  Each package is downloaded, validated and committed atomically.\n"
     "  Group installs are sequential and not atomic as a whole.\n"
     "  Installing an already valid package leaves it unchanged."},
    {"remove",
     "remove [<component>] <tool>[@<release>] [--target <target-platform>]",
     "Remove one installed release.",
     "Description:\n  Remove one installed package version and update its commands.\n"
     "Arguments:\n  component       Optional explicit installed component.\n"
     "  tool[@release]  Installed selection; release may be omitted only when unique.\n"
     "Options:\n  --target <target-platform>  Select the target platform.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "  Without a release, removes the package only when exactly one installed version matches.\n"
     "  Otherwise, lists the installed releases and requires one explicitly.\n"
     "Examples:\n  cup remove clang\n  cup remove clang@22.1.5\n"
     "  cup remove compiler clang@22.1.5\n"
     "Effects:\n  Removes the selected package and updates defaults and provided commands."},
    {"update",
     "update [cup|<tool>|<component>]",
     "Update installed tools or the cup executable.",
     "Description:\n  Update installed tools, or update the cup executable when cup is selected.\n"
     "Arguments:\n  cup|tool|component  Optional update selector.\n"
     "Options:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Without a selector, updates installed tools only; cup itself is not updated.\n"
     "Examples:\n  cup update\n  cup update clang\n  cup update compiler\n  cup update cup\n"
     "Effects:\n  Tool updates retain old releases.\n"
     "  A default moves only from an older release of the same tool.\n"
     "  cup update cup installs only a newer verified official release."},
    {"config",
     "config [--target <target-platform>] | config set <component> <tool> "
     "[--target <target-platform>] | config reset [<component>] [--target <target-platform>]",
     "Show or modify install preferences.",
     "Description:\n  Show effective tools or set/reset installation preferences.\n"
     "Arguments:\n  component  Component whose preference is changed.\n  tool  Preferred tool.\n"
     "Options:\n  --target <target-platform>  Select the target platform.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Uses the current host and target; reset without component clears preferences "
     "for that target.\n"
     "Examples:\n  cup config\n  cup config set compiler gcc\n  cup config reset compiler --target "
     "linux-x64\n"
     "Effects:\n  The view is read-only; set/reset update installation preferences."},
    {"default",
     "default <component> <tool>@<release> [--target <target-platform>]",
     "Select one installed package as the default.",
     "Description:\n  Set the default installed package for one component and target.\n"
     "Arguments:\n  component       Installed component.\n"
     "  tool@release    Installed selector; stable resolves through the catalog.\n"
     "Options:\n  --target <target-platform>  Select the target platform.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "Examples:\n  cup default compiler clang@stable\n"
     "  cup default compiler clang@22.1.5\n"
     "Effects:\n  Resolves an installed package, then updates the default and provided commands.\n"
     "  It never installs a missing package."},
    {"info",
     "info [<component>] [--target <target-platform>]",
     "Show defaults and their provided commands.",
     "Description:\n  Show defaults and their provided commands for the current host.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict output to one target.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Shows every target when --target is omitted.\n"
     "Examples:\n  cup info\n  cup info compiler --target linux-x64\n"
     "Effects:\n  Read-only; invalid defaults or provided commands are reported and return a "
     "nonzero status."},
    {"inspect",
     "inspect <component> <tool>@<release> [--target <target-platform>]",
     "Inspect an installed package.",
     "Description:\n  Validate one installed package and print its metadata.\n"
     "Arguments:\n  component       Installed component.\n"
     "  tool@release    Installed selector; stable resolves through the catalog.\n"
     "Options:\n  --target <target-platform>  Select the target platform.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "Examples:\n  cup inspect compiler clang@stable\n"
     "  cup inspect compiler clang@22.1.5\n"
     "Effects:\n  Read-only; never installs a missing package or initializes the local cup "
     "installation."},
    {"doctor",
     "doctor",
     "Diagnose cup without modifying files.",
     "Description:\n  Check the installation, state, packages and commands provided by defaults.\n"
     "Arguments:\n  None.\nOptions:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Checks the current user's cup installation.\n"
     "Examples:\n  cup doctor\n"
     "Effects:\n  Strictly read-only; never initializes the local cup installation."},
    {"repair",
     "repair",
     "Repair recoverable installation state.",
     "Description:\n  Recover interrupted operations and rebuild data that can be derived safely.\n"
     "Arguments:\n  None.\nOptions:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Repairs the current user's cup installation.\n"
     "Examples:\n  cup repair\n"
     "Effects:\n  May repair configuration, state, packages and commands provided by defaults."},
    {"uninstall",
     "uninstall [--yes]",
     "Remove cup and all managed data.",
     "Description:\n  Remove the selected cup installation without changing PATH.\n"
     "Arguments:\n  None.\nOptions:\n  --yes  Skip the confirmation prompt.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Prompts before removal.\n"
     "Examples:\n  cup uninstall\n  cup uninstall --yes\n"
     "Effects:\n  Removes cup and every cup-managed package; PATH is unchanged."}};

static CupError normalize_selector(const char *input, char *output, size_t output_size) {
    char tool[MAX_IDENTIFIER_LEN];
    char release[MAX_IDENTIFIER_LEN];
    char normalized_release[MAX_IDENTIFIER_LEN];
    CupError err;

    err = package_selector_parse_parts(input, tool, sizeof(tool), release, sizeof(release));
    if (err == CUP_OK) {
        err = text_copy_lower_ascii(tool, sizeof(tool), tool);
    }
    if (err == CUP_OK) {
        err = text_copy_lower_ascii(normalized_release, sizeof(normalized_release), release);
    }
    if (err == CUP_OK) {
        err = strcmp(normalized_release, "stable") == 0
                  ? text_copy(release, sizeof(release), "stable")
                  : package_release_validate_concrete(release);
    }
    return err == CUP_OK ? package_selector_format_parts(output, output_size, tool, release) : err;
}

static CupError normalize_optional_release_selector(const char *input,
                                                    char *output,
                                                    size_t output_size) {
    CupError err;

    if (text_is_empty(input) || output == NULL || output_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strchr(input, '@') != NULL) {
        return normalize_selector(input, output, output_size);
    }
    err = text_copy_lower_ascii(output, output_size, input);
    if (err != CUP_OK) {
        return err;
    }
    return path_is_safe_identifier(output) ? CUP_OK : CUP_ERR_INVALID_TOOL;
}

static CupError normalize_install_package_selector(const char *input,
                                                   char *output,
                                                   size_t output_size) {
    char tool[MAX_IDENTIFIER_LEN];
    CupError err;

    if (text_is_empty(input) || output == NULL || output_size == 0) {
        return CUP_ERR_INVALID_INPUT;
    }
    if (strchr(input, '@') != NULL) {
        return normalize_selector(input, output, output_size);
    }
    err = text_copy_lower_ascii(tool, sizeof(tool), input);
    if (err != CUP_OK) {
        return err;
    }
    if (!path_is_safe_identifier(tool)) {
        return CUP_ERR_INVALID_TOOL;
    }
    return package_selector_format_parts(output, output_size, tool, "stable");
}

static const CommandHelp *find_help(const char *name) {
    size_t i;
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        if (strcmp(COMMAND_HELP[i].name, name) == 0) {
            return &COMMAND_HELP[i];
        }
    }
    return NULL;
}

static void print_command_usage(FILE *stream, const CommandHelp *help) {
    const char *usage = help->usage;
    const char *separator;

    while ((separator = strstr(usage, " | ")) != NULL) {
        fprintf(stream, "  cup ");
        fwrite(usage, 1, (size_t)(separator - usage), stream);
        fputc('\n', stream);
        usage = separator + 3;
    }
    fprintf(stream, "  cup %s\n", usage);
}

static void print_usage(FILE *stream) {
    size_t i;
    fprintf(stream, "Usage:\n  cup --version\n");
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        print_command_usage(stream, &COMMAND_HELP[i]);
    }
}

static void print_help(void) {
    size_t i;
    print_usage(stdout);
    fprintf(stdout, "\nCommands:\n");
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        fprintf(stdout, "  %-12s %s\n", COMMAND_HELP[i].name, COMMAND_HELP[i].summary);
    }
    fprintf(stdout,
            "\nPackage selector:\n  <tool>@<release>\n"
            "\nExamples:\n"
            "  cup search compiler\n"
            "  cup install gcc\n"
            "  cup install compiler gcc@stable\n"
            "  cup update compiler\n"
            "  cup default compiler gcc@stable\n"
            "  cup info\n"
            "  cup inspect compiler gcc@stable\n");
}

static int print_detailed_help(const char *command) {
    const CommandHelp *help = find_help(command);
    if (help == NULL) {
        return 0;
    }
    fprintf(stdout, "Usage:\n");
    print_command_usage(stdout, help);
    fprintf(stdout, "\n%s\n", help->details);
    return 1;
}

static int argtable_is_complete(void *const *table, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (table[i] == NULL) {
            return 0;
        }
    }
    return 1;
}

static CupError report_parse_error(const char *command,
                                   struct arg_end *end,
                                   int errors) {
    const CommandHelp *help = find_help(command);
    if (errors > 0) {
        arg_print_errors(stderr, end, "cup");
    }
    if (help != NULL) {
        fprintf(stderr, "Usage:\n");
        print_command_usage(stderr, help);
    }
    return CUP_ERR_INVALID_INPUT;
}

/* Command-specific Argtable3 schemas produce one bounded typed command before runtime preflight. */
typedef enum {
    PUBLIC_COMMAND_SEARCH,
    PUBLIC_COMMAND_LIST,
    PUBLIC_COMMAND_INSTALL,
    PUBLIC_COMMAND_REMOVE,
    PUBLIC_COMMAND_UPDATE,
    PUBLIC_COMMAND_CONFIG,
    PUBLIC_COMMAND_DEFAULT,
    PUBLIC_COMMAND_INFO,
    PUBLIC_COMMAND_INSPECT,
    PUBLIC_COMMAND_DOCTOR,
    PUBLIC_COMMAND_REPAIR,
    PUBLIC_COMMAND_UNINSTALL
} PublicCommandKind;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
} OptionalComponentArguments;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char selector[MAX_SELECTOR_LEN];
    char target[MAX_PLATFORM_LEN];
} ComponentEntryArguments;

typedef struct {
    char selector[MAX_SELECTOR_LEN];
    char value[MAX_SELECTOR_LEN];
    char target[MAX_PLATFORM_LEN];
    char format[MAX_IDENTIFIER_LEN];
} InstallArguments;

typedef struct {
    char component[MAX_IDENTIFIER_LEN];
    char selector[MAX_SELECTOR_LEN];
    char target[MAX_PLATFORM_LEN];
} RemoveArguments;

typedef struct {
    char selector[MAX_IDENTIFIER_LEN];
} UpdateArguments;

typedef struct {
    char action[MAX_IDENTIFIER_LEN];
    char name[MAX_IDENTIFIER_LEN];
    char value[MAX_IDENTIFIER_LEN];
    char target[MAX_PLATFORM_LEN];
} ConfigArguments;

typedef struct {
    PublicCommandKind kind;
    union {
        OptionalComponentArguments optional_component;
        ComponentEntryArguments component_entry;
        InstallArguments install;
        RemoveArguments remove;
        UpdateArguments update;
        ConfigArguments config;
        int uninstall_assume_yes;
    } arguments;
} ParsedCommand;

static const char *optional_value(const char *value) {
    return text_is_empty(value) ? NULL : value;
}

static CupError report_value_error(const char *command, CupError error) {
    const CommandHelp *help = find_help(command);

    if (error == CUP_OK) {
        return CUP_OK;
    }
    if (error == CUP_ERR_BUFFER_TOO_SMALL) {
        fprintf(stderr, "Error: one or more command arguments exceed their supported length.\n");
        error = CUP_ERR_INVALID_INPUT;
    }
    if (help != NULL) {
        fprintf(stderr, "Usage:\n");
        print_command_usage(stderr, help);
    }
    return error;
}

static CupError report_command_shape_error(const char *command, const char *message) {
    if (!text_is_empty(message)) {
        fprintf(stderr, "Error: %s\n", message);
    }
    return report_value_error(command, CUP_ERR_INVALID_INPUT);
}

static CupError copy_public_value(const char *command,
                                  char *buffer,
                                  size_t size,
                                  const char *value,
                                  int lower_ascii) {
    CupError err;

    if (value == NULL) {
        if (buffer == NULL || size == 0) {
            return CUP_ERR_INVALID_INPUT;
        }
        buffer[0] = '\0';
        return CUP_OK;
    }
    err = lower_ascii ? text_copy_lower_ascii(buffer, size, value)
                      : text_copy(buffer, size, value);
    return report_value_error(command, err);
}

static CupError validate_public_target(const char *command, const char *target) {
    return text_is_empty(target) ? CUP_OK : report_value_error(command, platform_validate(target));
}

static CupError selector_tool_name(const char *selector, char *tool, size_t tool_size) {
    char release[MAX_IDENTIFIER_LEN];

    if (strchr(selector, '@') == NULL) {
        return text_copy(tool, tool_size, selector);
    }
    return package_selector_parse_parts(selector, tool, tool_size, release, sizeof(release));
}

static CupError validate_component_selector(const char *command,
                                            const char *component,
                                            const char *selector) {
    char tool[MAX_IDENTIFIER_LEN];
    CupError err;

    err = registry_validate_component(component);
    if (err == CUP_OK) {
        err = selector_tool_name(selector, tool, sizeof(tool));
    }
    if (err == CUP_OK) {
        err = registry_validate_tool(component, tool);
    }
    return report_value_error(command, err);
}

static CupError resolve_unscoped_selector_component(const char *command,
                                                    const char *selector,
                                                    char *component,
                                                    size_t component_size) {
    char tool[MAX_IDENTIFIER_LEN];
    CupError err;

    err = selector_tool_name(selector, tool, sizeof(tool));
    if (err == CUP_OK) {
        err = registry_find_tool_component(tool, component, component_size);
    }
    return report_value_error(command, err);
}

static CupError normalize_install_arguments(char *selector,
                                            size_t selector_size,
                                            char *value,
                                            size_t value_size) {
    char key[MAX_IDENTIFIER_LEN];
    char normalized[MAX_SELECTOR_LEN];
    char tool[MAX_IDENTIFIER_LEN];
    char component[MAX_IDENTIFIER_LEN];
    CupError err;

    err = text_copy_lower_ascii(key, sizeof(key), selector);
    if (err != CUP_OK) {
        return err;
    }
    if (registry_is_component(key)) {
        err = text_copy(selector, selector_size, key);
        if (err == CUP_OK && !text_is_empty(value)) {
            err = normalize_install_package_selector(value, normalized, sizeof(normalized));
            if (err == CUP_OK) {
                err = selector_tool_name(normalized, tool, sizeof(tool));
            }
            if (err == CUP_OK) {
                err = registry_validate_tool(key, tool);
            }
            if (err == CUP_OK) {
                err = text_copy(value, value_size, normalized);
            }
        }
        return err;
    }
    if (strcmp(key, "profile") == 0 || strcmp(key, "toolchain") == 0) {
        if (text_is_empty(value)) {
            return CUP_ERR_INVALID_INPUT;
        }
        err = text_copy_lower_ascii(value, value_size, value);
        if (err == CUP_OK && !path_is_safe_identifier(value)) {
            err = CUP_ERR_INVALID_INPUT;
        }
        if (err == CUP_OK) {
            err = text_copy(selector, selector_size, key);
        }
        return err;
    }
    if (!text_is_empty(value)) {
        return CUP_ERR_UNSUPPORTED_COMPONENT;
    }
    err = normalize_install_package_selector(selector, normalized, sizeof(normalized));
    if (err == CUP_OK) {
        err = selector_tool_name(normalized, tool, sizeof(tool));
    }
    if (err == CUP_OK) {
        err = registry_find_tool_component(tool, component, sizeof(component));
    }
    return err == CUP_OK ? text_copy(selector, selector_size, normalized) : err;
}

static CupError parse_optional_component(const char *command,
                                         PublicCommandKind kind,
                                         int argc,
                                         char **argv,
                                         ParsedCommand *parsed) {
    struct arg_str *component = arg_str0(NULL, NULL, "<component>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, target, end};
    OptionalComponentArguments *arguments = &parsed->arguments.optional_component;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(command, end, errors);
    } else {
        result = copy_public_value(command,
                                   arguments->component,
                                   sizeof(arguments->component),
                                   component->count ? component->sval[0] : NULL,
                                   1);
        if (result == CUP_OK) {
            result = copy_public_value(command,
                                       arguments->target,
                                       sizeof(arguments->target),
                                       target->count ? target->sval[0] : NULL,
                                       1);
        }
        if (result == CUP_OK && !text_is_empty(arguments->component)) {
            result = report_value_error(command,
                                        registry_validate_component(arguments->component));
        }
        if (result == CUP_OK) {
            result = validate_public_target(command, arguments->target);
        }
        if (result == CUP_OK) {
            parsed->kind = kind;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_component_entry(const char *command,
                                      PublicCommandKind kind,
                                      int argc,
                                      char **argv,
                                      ParsedCommand *parsed) {
    struct arg_str *component = arg_str1(NULL, NULL, "<component>", NULL);
    struct arg_str *selector = arg_str1(NULL, NULL, "<tool>@<release>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, selector, target, end};
    ComponentEntryArguments *arguments = &parsed->arguments.component_entry;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    if (argc < 4) {
        fprintf(stderr,
                "Error: %s requires <component> and <tool>@<release>.\n",
                command);
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return report_value_error(command, CUP_ERR_INVALID_INPUT);
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(command, end, errors);
    } else {
        result = copy_public_value(command,
                                   arguments->component,
                                   sizeof(arguments->component),
                                   component->sval[0],
                                   1);
        if (result == CUP_OK) {
            result = report_value_error(
                command,
                normalize_selector(selector->sval[0],
                                   arguments->selector,
                                   sizeof(arguments->selector)));
        }
        if (result == CUP_OK) {
            result = copy_public_value(command,
                                       arguments->target,
                                       sizeof(arguments->target),
                                       target->count ? target->sval[0] : NULL,
                                       1);
        }
        if (result == CUP_OK) {
            result = validate_component_selector(
                command, arguments->component, arguments->selector);
        }
        if (result == CUP_OK) {
            result = validate_public_target(command, arguments->target);
        }
        if (result == CUP_OK) {
            parsed->kind = kind;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_install(int argc, char **argv, ParsedCommand *parsed) {
    struct arg_str *selector =
        arg_str1(NULL, NULL, "<component|tool[@release]|profile|toolchain>", NULL);
    struct arg_str *value = arg_str0(NULL, NULL, "[tool[@release]|name]", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_str *format = arg_str0("f", "format", "<archive-format>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {selector, value, target, format, end};
    InstallArguments *arguments = &parsed->arguments.install;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error("install", end, errors);
    } else {
        result = copy_public_value(
            "install", arguments->selector, sizeof(arguments->selector), selector->sval[0], 0);
        if (result == CUP_OK) {
            result = copy_public_value("install",
                                       arguments->value,
                                       sizeof(arguments->value),
                                       value->count ? value->sval[0] : NULL,
                                       0);
        }
        if (result == CUP_OK) {
            result = copy_public_value("install",
                                       arguments->target,
                                       sizeof(arguments->target),
                                       target->count ? target->sval[0] : NULL,
                                       1);
        }
        if (result == CUP_OK) {
            result = copy_public_value("install",
                                       arguments->format,
                                       sizeof(arguments->format),
                                       format->count ? format->sval[0] : NULL,
                                       1);
        }
        if (result == CUP_OK) {
            result = report_value_error(
                "install",
                normalize_install_arguments(arguments->selector,
                                            sizeof(arguments->selector),
                                            arguments->value,
                                            sizeof(arguments->value)));
        }
        if (result == CUP_OK) {
            result = validate_public_target("install", arguments->target);
        }
        if (result == CUP_OK && !text_is_empty(arguments->format)) {
            PackageArchiveFormat archive_format;

            result = report_value_error(
                "install", package_archive_parse_format(arguments->format, &archive_format));
        }
        if (result == CUP_OK) {
            parsed->kind = PUBLIC_COMMAND_INSTALL;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_remove(int argc, char **argv, ParsedCommand *parsed) {
    struct arg_str *first = arg_str1(NULL, NULL, "<component|tool[@release]>", NULL);
    struct arg_str *second = arg_str0(NULL, NULL, "[tool[@release]]", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {first, second, target, end};
    RemoveArguments *arguments = &parsed->arguments.remove;
    const char *selector_input;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error("remove", end, errors);
    } else {
        selector_input = first->sval[0];
        result = CUP_OK;
        if (second->count != 0) {
            result = copy_public_value("remove",
                                       arguments->component,
                                       sizeof(arguments->component),
                                       first->sval[0],
                                       1);
            selector_input = second->sval[0];
        }
        if (result == CUP_OK) {
            result = report_value_error(
                "remove",
                normalize_optional_release_selector(
                    selector_input, arguments->selector, sizeof(arguments->selector)));
        }
        if (result == CUP_OK) {
            result = copy_public_value("remove",
                                       arguments->target,
                                       sizeof(arguments->target),
                                       target->count ? target->sval[0] : NULL,
                                       1);
        }
        if (result == CUP_OK) {
            result = text_is_empty(arguments->component)
                         ? resolve_unscoped_selector_component("remove",
                                                               arguments->selector,
                                                               arguments->component,
                                                               sizeof(arguments->component))
                         : validate_component_selector(
                               "remove", arguments->component, arguments->selector);
        }
        if (result == CUP_OK) {
            result = validate_public_target("remove", arguments->target);
        }
        if (result == CUP_OK) {
            parsed->kind = PUBLIC_COMMAND_REMOVE;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_update(int argc, char **argv, ParsedCommand *parsed) {
    struct arg_str *selector = arg_str0(NULL, NULL, "[cup|tool|component]", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {selector, end};
    UpdateArguments *arguments = &parsed->arguments.update;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error("update", end, errors);
    } else {
        result = copy_public_value("update",
                                   arguments->selector,
                                   sizeof(arguments->selector),
                                   selector->count ? selector->sval[0] : NULL,
                                   1);
        if (result == CUP_OK && !text_is_empty(arguments->selector) &&
            strcmp(arguments->selector, "cup") != 0 &&
            !registry_is_component(arguments->selector)) {
            char component[MAX_IDENTIFIER_LEN];

            result = report_value_error(
                "update",
                registry_find_tool_component(
                    arguments->selector, component, sizeof(component)));
        }
        if (result == CUP_OK) {
            parsed->kind = PUBLIC_COMMAND_UPDATE;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError copy_config_values(const struct arg_str *action,
                                   const struct arg_str *name,
                                   const struct arg_str *value,
                                   const struct arg_str *target,
                                   ConfigArguments *arguments) {
    CupError result;

    result = copy_public_value("config",
                               arguments->action,
                               sizeof(arguments->action),
                               action->count ? action->sval[0] : NULL,
                               1);
    if (result == CUP_OK) {
        result = copy_public_value("config",
                                   arguments->name,
                                   sizeof(arguments->name),
                                   name->count ? name->sval[0] : NULL,
                                   1);
    }
    if (result == CUP_OK) {
        result = copy_public_value("config",
                                   arguments->value,
                                   sizeof(arguments->value),
                                   value->count ? value->sval[0] : NULL,
                                   1);
    }
    if (result == CUP_OK) {
        result = copy_public_value("config",
                                   arguments->target,
                                   sizeof(arguments->target),
                                   target->count ? target->sval[0] : NULL,
                                   1);
    }
    return result;
}

static CupError validate_config_values(const ConfigArguments *arguments) {
    if (text_is_empty(arguments->action)) {
        return text_is_empty(arguments->name) && text_is_empty(arguments->value)
                   ? CUP_OK
                   : report_command_shape_error(
                         "config", "config view does not accept positional values");
    }

    if (strcmp(arguments->action, "set") == 0) {
        if (text_is_empty(arguments->name) || text_is_empty(arguments->value)) {
            return report_command_shape_error(
                "config", "config set requires <component> and <tool>");
        }
        return report_value_error(
            "config", registry_validate_tool(arguments->name, arguments->value));
    }

    if (strcmp(arguments->action, "reset") == 0) {
        if (!text_is_empty(arguments->value)) {
            return report_command_shape_error(
                "config", "config reset accepts at most one <component>");
        }
        return text_is_empty(arguments->name)
                   ? CUP_OK
                   : report_value_error("config",
                                        registry_validate_component(arguments->name));
    }

    return report_command_shape_error(
        "config", "config action must be 'set' or 'reset'");
}

static CupError parse_config(int argc, char **argv, ParsedCommand *parsed) {
    struct arg_str *action = arg_str0(NULL, NULL, "[set|reset]", NULL);
    struct arg_str *name = arg_str0(NULL, NULL, "[component]", NULL);
    struct arg_str *value = arg_str0(NULL, NULL, "[tool]", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {action, name, value, target, end};
    ConfigArguments *arguments = &parsed->arguments.config;
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error("config", end, errors);
    } else {
        result = copy_config_values(action, name, value, target, arguments);
        if (result == CUP_OK) {
            result = validate_config_values(arguments);
        }
        if (result == CUP_OK) {
            result = validate_public_target("config", arguments->target);
        }
        if (result == CUP_OK) {
            parsed->kind = PUBLIC_COMMAND_CONFIG;
        }
    }

    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_no_arguments(const char *command,
                                   PublicCommandKind kind,
                                   int argc,
                                   char **argv,
                                   ParsedCommand *parsed) {
    struct arg_end *end = arg_end(4);
    void *table[] = {end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    result = errors != 0 ? report_parse_error(command, end, errors) : CUP_OK;
    if (result == CUP_OK) {
        parsed->kind = kind;
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_uninstall(int argc, char **argv, ParsedCommand *parsed) {
    struct arg_lit *yes = arg_lit0(NULL, "yes", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {yes, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    result = errors != 0 ? report_parse_error("uninstall", end, errors) : CUP_OK;
    if (result == CUP_OK) {
        parsed->kind = PUBLIC_COMMAND_UNINSTALL;
        parsed->arguments.uninstall_assume_yes = yes->count != 0;
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}


static CupError parse_help(int argc, char **argv) {
    struct arg_str *command = arg_str0(NULL, NULL, "[command]", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {command, end};
    int errors;
    CupError result = CUP_OK;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error("help", end, errors);
    } else if (command->count == 0) {
        print_help();
    } else if (!print_detailed_help(command->sval[0])) {
        fprintf(stderr, "Error: unknown command '%s'.\n", command->sval[0]);
        result = CUP_ERR_INVALID_INPUT;
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_public_command(const char *command,
                                     int argc,
                                     char **argv,
                                     ParsedCommand *parsed) {
    if (parsed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }
    memset(parsed, 0, sizeof(*parsed));

    if (strcmp(command, "search") == 0) {
        return parse_optional_component(
            command, PUBLIC_COMMAND_SEARCH, argc, argv, parsed);
    }
    if (strcmp(command, "list") == 0) {
        return parse_optional_component(
            command, PUBLIC_COMMAND_LIST, argc, argv, parsed);
    }
    if (strcmp(command, "install") == 0) {
        return parse_install(argc, argv, parsed);
    }
    if (strcmp(command, "remove") == 0) {
        return parse_remove(argc, argv, parsed);
    }
    if (strcmp(command, "update") == 0) {
        return parse_update(argc, argv, parsed);
    }
    if (strcmp(command, "config") == 0) {
        return parse_config(argc, argv, parsed);
    }
    if (strcmp(command, "default") == 0) {
        return parse_component_entry(
            command, PUBLIC_COMMAND_DEFAULT, argc, argv, parsed);
    }
    if (strcmp(command, "info") == 0) {
        return parse_optional_component(
            command, PUBLIC_COMMAND_INFO, argc, argv, parsed);
    }
    if (strcmp(command, "inspect") == 0) {
        return parse_component_entry(
            command, PUBLIC_COMMAND_INSPECT, argc, argv, parsed);
    }
    if (strcmp(command, "doctor") == 0) {
        return parse_no_arguments(
            command, PUBLIC_COMMAND_DOCTOR, argc, argv, parsed);
    }
    if (strcmp(command, "repair") == 0) {
        return parse_no_arguments(
            command, PUBLIC_COMMAND_REPAIR, argc, argv, parsed);
    }
    if (strcmp(command, "uninstall") == 0) {
        return parse_uninstall(argc, argv, parsed);
    }
    return CUP_ERR_INVALID_INPUT;
}

static CupError execute_public_command(const ParsedCommand *parsed) {
    const OptionalComponentArguments *optional;
    const ComponentEntryArguments *entry;

    if (parsed == NULL) {
        return CUP_ERR_INVALID_INPUT;
    }

    switch (parsed->kind) {
        case PUBLIC_COMMAND_SEARCH:
            optional = &parsed->arguments.optional_component;
            return command_search(optional_value(optional->component),
                                  optional_value(optional->target));
        case PUBLIC_COMMAND_LIST:
            optional = &parsed->arguments.optional_component;
            return command_list(optional_value(optional->component),
                                optional_value(optional->target));
        case PUBLIC_COMMAND_INSTALL:
            return command_install(parsed->arguments.install.selector,
                                   optional_value(parsed->arguments.install.value),
                                   optional_value(parsed->arguments.install.target),
                                   optional_value(parsed->arguments.install.format));
        case PUBLIC_COMMAND_REMOVE:
            return command_remove(optional_value(parsed->arguments.remove.component),
                                  parsed->arguments.remove.selector,
                                  optional_value(parsed->arguments.remove.target));
        case PUBLIC_COMMAND_UPDATE:
            return command_update(optional_value(parsed->arguments.update.selector));
        case PUBLIC_COMMAND_CONFIG:
            return command_config(optional_value(parsed->arguments.config.action),
                                  optional_value(parsed->arguments.config.name),
                                  optional_value(parsed->arguments.config.value),
                                  optional_value(parsed->arguments.config.target));
        case PUBLIC_COMMAND_DEFAULT:
            entry = &parsed->arguments.component_entry;
            return command_default(
                entry->component, entry->selector, optional_value(entry->target));
        case PUBLIC_COMMAND_INFO:
            optional = &parsed->arguments.optional_component;
            return command_info(optional_value(optional->component),
                                optional_value(optional->target));
        case PUBLIC_COMMAND_INSPECT:
            entry = &parsed->arguments.component_entry;
            return command_inspect(
                entry->component, entry->selector, optional_value(entry->target));
        case PUBLIC_COMMAND_DOCTOR:
            return command_doctor();
        case PUBLIC_COMMAND_REPAIR:
            return command_repair();
        case PUBLIC_COMMAND_UNINSTALL:
            return command_uninstall(parsed->arguments.uninstall_assume_yes);
    }

    return CUP_ERR_INVALID_INPUT;
}

static int command_uses_interrupt(const ParsedCommand *parsed) {
    if (parsed == NULL) {
        return 0;
    }

    switch (parsed->kind) {
        case PUBLIC_COMMAND_INSTALL:
        case PUBLIC_COMMAND_REMOVE:
        case PUBLIC_COMMAND_UPDATE:
        case PUBLIC_COMMAND_DEFAULT:
        case PUBLIC_COMMAND_REPAIR:
        case PUBLIC_COMMAND_UNINSTALL:
            return 1;
        case PUBLIC_COMMAND_CONFIG:
            return strcmp(parsed->arguments.config.action, "set") == 0 ||
                   strcmp(parsed->arguments.config.action, "reset") == 0;
        case PUBLIC_COMMAND_SEARCH:
        case PUBLIC_COMMAND_LIST:
        case PUBLIC_COMMAND_INFO:
        case PUBLIC_COMMAND_INSPECT:
        case PUBLIC_COMMAND_DOCTOR:
            return 0;
        default:
            return 0;
    }
}

/* Top-level command dispatch. */
#ifdef CUP_COVERAGE_ENTRY
#define main CUP_COVERAGE_ENTRY
#endif

int main(int argc, char *argv[]) {
    const char *command;
    const CommandHelp *help;
    ParsedCommand parsed;
    CupError result;
    int interrupt_active = 0;

    /* Keep newline-terminated progress ahead of stderr diagnostics in redirected logs. */
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    system_set_restrictive_umask();

    /* Internal bootstrap validates a complete transport directory before entering the canonical
     * root lock, journal, staging and detached-helper protocol. */
    if (argc == 3 && strcmp(argv[1], "--internal-bootstrap") == 0) {
        int status;

        result = layout_root_snapshot_begin();
        if (result == CUP_OK) {
            result = interrupt_enable();
        }
        if (result == CUP_OK) {
            result = bootstrap_start(argv[2], argv[0]);
        }
        interrupt_disable();
        status = exit_status_from_error(result);
        layout_root_snapshot_end();
        return status;
    }

    /* Internal helper modes bypass the public CLI. Handoff acceptance waits for parent exit and
     * carries exclusive authority before either helper mutates managed state. */
    if (argc == 6 && strcmp(argv[1], "--internal-update-helper") == 0) {
        result = update_helper_run(argv[2], argv[3], argv[4], argv[5]);
        return exit_status_from_error(result);
    }
    if (argc == 7 && strcmp(argv[1], "--internal-uninstall-helper") == 0) {
        result = uninstall_helper_run(argv[2], argv[3], argv[4], argv[5], argv[6]);
        return exit_status_from_error(result);
    }
    if (argc < 2) {
        print_usage(stderr);
        return CUP_STATUS_USAGE;
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_help();
        return CUP_STATUS_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("cup %s\n", CUP_VERSION);
        return CUP_STATUS_SUCCESS;
    }

    command = argv[1];
    help = find_help(command);
    if (argc == 3 && help != NULL &&
        (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)) {
        print_detailed_help(command);
        return CUP_STATUS_SUCCESS;
    }
    if (help == NULL) {
        fprintf(stderr, "Error: unknown command '%s'.\n", command);
        print_usage(stderr);
        return CUP_STATUS_USAGE;
    }
    if (strcmp(command, "help") == 0) {
        return exit_status_from_error(parse_help(argc, argv));
    }

    result = parse_public_command(command, argc, argv, &parsed);
    if (result != CUP_OK) {
        return exit_status_from_error(result);
    }

    /* Doctor diagnoses root candidates itself; every other command receives one root snapshot. */
    if (parsed.kind != PUBLIC_COMMAND_DOCTOR) {
        result = layout_root_snapshot_begin();
        if (result != CUP_OK) {
            return exit_status_from_error(result);
        }
    }

    /* Install native interrupt handling only after validating the state-changing command form. */
    if (command_uses_interrupt(&parsed)) {
        result = interrupt_enable();
        if (result != CUP_OK) {
            fprintf(stderr, "Error: native interrupt handling could not be enabled.\n");
            layout_root_snapshot_end();
            return exit_status_from_error(result);
        }
        interrupt_active = 1;
    }

    result = execute_public_command(&parsed);

    if (interrupt_active) {
        interrupt_disable();
    }
    layout_root_snapshot_end();
    return exit_status_from_error(result);
}

#ifdef CUP_COVERAGE_ENTRY
#undef main
#endif

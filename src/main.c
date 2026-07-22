/*
 * Defines command help, Argtable3 argument schemas and the top-level command dispatcher. Runtime
 * behavior remains in command modules.
 */

#include "commands.h"

#include "error.h"
#include "exit_status.h"
#include "interrupt.h"
#include "cup_update_helper.h"
#include "cup_update_journal.h"
#include "runtime_journal.h"
#include "system.h"
#include "package_selector.h"
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
     "Effects:\n  Read-only; never initializes the cup runtime."},
    {"search",
     "search [<component>] [--target <target-platform>]",
     "Search the package catalog.",
     "Description:\n  Show packages available from the active catalog.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict results to one target.\n"
     "  -h, --help                   Show this help.\n"
     "Defaults:\n  Uses the current host and shows every target when --target is omitted.\n"
     "Examples:\n  cup search\n  cup search compiler --target linux-x64\n"
     "Effects:\n  Read-only; never initializes the cup runtime."},
    {"list",
     "list [<component>] [--target <target-platform>]",
     "List installed packages.",
     "Description:\n  Show installed package versions and default/stable annotations.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict results to one target.\n"
     "  -h, --help                   Show this help.\n"
     "Defaults:\n  Uses the current host and all installed targets.\n"
     "Examples:\n  cup list\n  cup list compiler --target linux-x64\n"
     "Effects:\n  Read-only; missing or invalid package entries produce degraded output and a "
     "nonzero status."},
    {"install",
     "install <component> [<tool>[@<release>]] [--target <target-platform>] "
     "[--format|-f <archive-format>] | install <profile|toolchain> <name> "
     "[--target <target-platform>] [--format|-f <archive-format>]",
     "Install one package, profile or toolchain.",
     "Description:\n  Resolve and install one package or a prevalidated group.\n"
     "Arguments:\n  component            Component to install.\n"
     "  tool[@release]      Optional explicit selector.\n"
     "  profile|toolchain   Group kind followed by its name.\n"
     "Options:\n  --target <target-platform>  Select the target scope.\n"
     "  -f, --format <format>       Select an archive format.\n"
     "  -h, --help                  Show this help.\n"
     "Defaults:\n  Omitted tool resolves preferred > official default; omitted release means "
     "stable.\n"
     "Examples:\n  cup install compiler\n  cup install compiler gcc@stable\n  cup install profile "
     "standard\n  cup install toolchain llvm\n"
     "Effects:\n  Downloads, validates and atomically commits packages; an intact installed "
     "selection is idempotent."},
    {"remove",
     "remove <component> <tool>@<release> [--target <target-platform>]",
     "Remove one installed release.",
     "Description:\n  Remove one concrete installed package and reconcile wrappers.\n"
     "Arguments:\n  component       Installed component.\n  tool@release    Concrete installed "
     "selector.\n"
     "Options:\n  --target <target-platform>  Select the target scope.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "Examples:\n  cup remove compiler clang@22.1.5\n"
     "Effects:\n  Atomically updates state and wrappers; the selected package directory is "
     "removed."},
    {"update",
     "update [cup|<tool>|<component>]",
     "Update cup or installed package scopes.",
     "Description:\n  Install stable releases for matching installed tool scopes.\n"
     "Arguments:\n  cup|tool|component  Optional update selector.\n"
     "Options:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Without a selector, updates installed tools only; CUP itself is not updated.\n"
     "Examples:\n  cup update\n  cup update clang\n  cup update compiler\n  cup update cup\n"
     "Effects:\n  Retains old releases; moves a default only when it selected an older release of "
     "the same tool."},
    {"config",
     "config [--target <target-platform>] | config set <component> <tool> "
     "[--target <target-platform>] | config reset [<component>] [--target <target-platform>]",
     "Show or modify install preferences.",
     "Description:\n  Show effective scoped selections or set/reset preferred tools.\n"
     "Arguments:\n  component  Component whose preference is changed.\n  tool  Preferred tool.\n"
     "Options:\n  --target <target-platform>  Select the preference scope.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Uses the current host and target; reset without component clears that scope "
     "only.\n"
     "Examples:\n  cup config\n  cup config set compiler gcc\n  cup config reset compiler --target "
     "linux-x64\n"
     "Effects:\n  The view is read-only; set/reset atomically update preferences.txt for future "
     "installs."},
    {"default",
     "default <component> <tool>@<release> [--target <target-platform>]",
     "Select one installed package as the default.",
     "Description:\n  Set the public default for one installed component scope.\n"
     "Arguments:\n  component       Installed component.\n  tool@release    Concrete installed "
     "selector.\n"
     "Options:\n  --target <target-platform>  Select the target scope.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "Examples:\n  cup default compiler clang@22.1.5\n"
     "Effects:\n  Atomically updates state and rebuilds managed wrappers."},
    {"info",
     "info [<component>] [--target <target-platform>]",
     "Show defaults and managed wrappers.",
     "Description:\n  Show aggregate installed/default status for the current host.\n"
     "Arguments:\n  component  Optional component filter.\n"
     "Options:\n  --target <target-platform>  Restrict output to one target.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Shows every target when --target is omitted.\n"
     "Examples:\n  cup info\n  cup info compiler --target linux-x64\n"
     "Effects:\n  Read-only; invalid defaults or wrappers produce degraded output and a nonzero "
     "status."},
    {"inspect",
     "inspect <component> <tool>@<release> [--target <target-platform>]",
     "Inspect an installed package.",
     "Description:\n  Validate one installed package and print immutable info.txt metadata.\n"
     "Arguments:\n  component       Installed component.\n  tool@release    Concrete installed "
     "selector.\n"
     "Options:\n  --target <target-platform>  Select the target scope.\n  -h, --help  Show this "
     "help.\n"
     "Defaults:\n  Uses the current host and target.\n"
     "Examples:\n  cup inspect compiler clang@22.1.5\n"
     "Effects:\n  Read-only; never initializes the cup runtime."},
    {"doctor",
     "doctor",
     "Diagnose cup without modifying files.",
     "Description:\n  Check assets, state, packages, journals and wrappers.\n"
     "Arguments:\n  None.\nOptions:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Checks the current user's CUP installation.\n"
     "Examples:\n  cup doctor\n"
     "Effects:\n  Strictly read-only; never initializes the cup runtime."},
    {"repair",
     "repair",
     "Apply deterministic repairs.",
     "Description:\n  Recover interrupted operations and rebuild deterministic managed data.\n"
     "Arguments:\n  None.\nOptions:\n  -h, --help  Show this help.\n"
     "Defaults:\n  Repairs the current user's CUP installation.\n"
     "Examples:\n  cup repair\n"
     "Effects:\n  May modify state, packages, journals, configuration assets and wrappers."},
    {"uninstall",
     "uninstall [--yes]",
     "Remove cup and all managed data.",
     "Description:\n  Remove the canonical CUP root without changing PATH.\n"
     "Arguments:\n  None.\nOptions:\n  --yes  Skip the confirmation prompt.\n  -h, --help  Show "
     "this help.\n"
     "Defaults:\n  Prompts before removal.\n"
     "Examples:\n  cup uninstall\n  cup uninstall --yes\n"
     "Effects:\n  Starts a detached helper that removes CUP and every CUP-managed package."}};

static const char *program_name(const char *name) {
    return name == NULL ? "cup" : name;
}

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
    if (err == CUP_OK && strcmp(normalized_release, "stable") == 0) {
        err = text_copy(release, sizeof(release), "stable");
    }
    return err == CUP_OK ? package_selector_format_parts(output, output_size, tool, release) : err;
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

static void print_command_usage(FILE *stream, const char *program, const CommandHelp *help) {
    const char *usage = help->usage;
    const char *separator;

    while ((separator = strstr(usage, " | ")) != NULL) {
        fprintf(stream, "  %s ", program_name(program));
        fwrite(usage, 1, (size_t)(separator - usage), stream);
        fputc('\n', stream);
        usage = separator + 3;
    }
    fprintf(stream, "  %s %s\n", program_name(program), usage);
}

static void print_usage(FILE *stream, const char *program) {
    size_t i;
    fprintf(stream, "Usage:\n  %s --version\n", program_name(program));
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        print_command_usage(stream, program, &COMMAND_HELP[i]);
    }
}

static void print_help(const char *program) {
    size_t i;
    print_usage(stdout, program);
    fprintf(stdout, "\nCommands:\n");
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        fprintf(stdout, "  %-12s %s\n", COMMAND_HELP[i].name, COMMAND_HELP[i].summary);
    }
    fprintf(stdout,
            "\nPackage selector:\n  <tool>@<release>\n"
            "\nExamples:\n"
            "  %s search compiler\n"
            "  %s install compiler gcc@stable\n"
            "  %s update compiler\n"
            "  %s default compiler gcc@stable\n"
            "  %s info\n"
            "  %s inspect compiler gcc@stable\n",
            program_name(program),
            program_name(program),
            program_name(program),
            program_name(program),
            program_name(program),
            program_name(program));
}

static int print_detailed_help(const char *program, const char *command) {
    const CommandHelp *help = find_help(command);
    if (help == NULL) {
        return 0;
    }
    fprintf(stdout, "Usage:\n");
    print_command_usage(stdout, program, help);
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

static CupError report_parse_error(const char *program,
                                   const char *command,
                                   struct arg_end *end,
                                   int errors) {
    const CommandHelp *help = find_help(command);
    if (errors > 0) {
        arg_print_errors(stderr, end, program_name(program));
    }
    if (help != NULL) {
        fprintf(stderr, "Usage:\n");
        print_command_usage(stderr, program, help);
    }
    return CUP_ERR_INVALID_INPUT;
}

/* Command-specific Argtable3 schemas. */
static CupError parse_optional_component(const char *program,
                                         const char *command,
                                         int argc,
                                         char **argv,
                                         CupError (*handler)(const char *, const char *)) {
    struct arg_str *component = arg_str0(NULL, NULL, "<component>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, target, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(program, command, end, errors);
    } else if (component->count != 0) {
        char normalized_component[MAX_IDENTIFIER_LEN];

        result = text_copy_lower_ascii(
            normalized_component, sizeof(normalized_component), component->sval[0]);
        if (result == CUP_OK) {
            result = handler(normalized_component, target->count ? target->sval[0] : NULL);
        }
    } else {
        result = handler(NULL, target->count ? target->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_component_entry(const char *program,
                                      const char *command,
                                      int argc,
                                      char **argv,
                                      CupError (*handler)(const char *,
                                                          const char *,
                                                          const char *)) {
    struct arg_str *component = arg_str1(NULL, NULL, "<component>", NULL);
    struct arg_str *selector = arg_str1(NULL, NULL, "<tool>@<release>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, selector, target, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(program, command, end, errors);
    } else {
        char normalized_component[MAX_IDENTIFIER_LEN];
        char normalized_selector[MAX_SELECTOR_LEN];

        result = text_copy_lower_ascii(
            normalized_component, sizeof(normalized_component), component->sval[0]);
        if (result == CUP_OK) {
            result = normalize_selector(
                selector->sval[0], normalized_selector, sizeof(normalized_selector));
        }
        if (result == CUP_OK) {
            result = handler(
                normalized_component, normalized_selector, target->count ? target->sval[0] : NULL);
        }
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_install(const char *program, int argc, char **argv) {
    struct arg_str *selector = arg_str1(NULL, NULL, "<component|profile|toolchain>", NULL);
    struct arg_str *value = arg_str0(NULL, NULL, "[tool[@release]|name]", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_str *format = arg_str0("f", "format", "<archive-format>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {selector, value, target, format, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(program, "install", end, errors);
    } else {
        result = command_install_request(selector->sval[0],
                                         value->count ? value->sval[0] : NULL,
                                         target->count ? target->sval[0] : NULL,
                                         format->count ? format->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_update(const char *program, int argc, char **argv) {
    struct arg_str *selector = arg_str0(NULL, NULL, "[cup|tool|component]", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {selector, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(program, "update", end, errors);
    } else {
        result = command_update(selector->count ? selector->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_config(const char *program, int argc, char **argv) {
    struct arg_str *action = arg_str0(NULL, NULL, "[set|reset]", NULL);
    struct arg_str *name = arg_str0(NULL, NULL, "[component]", NULL);
    struct arg_str *value = arg_str0(NULL, NULL, "[tool]", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {action, name, value, target, end};
    int errors;
    CupError result;

    if (!argtable_is_complete(table, sizeof(table) / sizeof(table[0]))) {
        fprintf(stderr, "Error: not enough memory to parse arguments.\n");
        arg_freetable(table, sizeof(table) / sizeof(table[0]));
        return CUP_ERR_TEMPORARY;
    }

    errors = arg_parse(argc - 1, argv + 1, table);
    if (errors != 0) {
        result = report_parse_error(program, "config", end, errors);
    } else {
        result = command_config(action->count ? action->sval[0] : NULL,
                                name->count ? name->sval[0] : NULL,
                                value->count ? value->sval[0] : NULL,
                                target->count ? target->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_no_arguments(
    const char *program, const char *command, int argc, char **argv, CupError (*handler)(void)) {
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
    if (errors != 0) {
        result = report_parse_error(program, command, end, errors);
    } else {
        result = handler();
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_uninstall(const char *program, int argc, char **argv) {
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
    if (errors != 0) {
        result = report_parse_error(program, "uninstall", end, errors);
    } else {
        result = command_uninstall(yes->count != 0);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_help(const char *program, int argc, char **argv) {
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
        result = report_parse_error(program, "help", end, errors);
    } else if (command->count == 0) {
        print_help(program);
    } else if (!print_detailed_help(program, command->sval[0])) {
        fprintf(stderr, "Error: unknown command '%s'.\n", command->sval[0]);
        result = CUP_ERR_INVALID_INPUT;
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static int command_uses_interrupt(int argc, char **argv) {
    const char *command;

    if (argc < 2 || argv == NULL) {
        return 0;
    }
    /* Resolve help before touching runtime state so every help form remains read-only. */
    command = argv[1];
    if (strcmp(command, "install") == 0 || strcmp(command, "remove") == 0 ||
        strcmp(command, "update") == 0 || strcmp(command, "default") == 0 ||
        strcmp(command, "repair") == 0 || strcmp(command, "uninstall") == 0) {
        return 1;
    }
    return strcmp(command, "config") == 0 && argc >= 3 &&
           (strcmp(argv[2], "set") == 0 || strcmp(argv[2], "reset") == 0);
}

/* Top-level command dispatch. */
int main(int argc, char *argv[]) {
    system_set_restrictive_umask();
    const char *command;
    const CommandHelp *help;
    CupError result;
    int interrupt_active = 0;

    /* Internal helper mode bypasses the public CLI and runs only the deferred update protocol. */
    if (argc == 4 && strcmp(argv[1], "--internal-cup-update-helper") == 0) {
        return cup_error_to_exit_status(cup_update_helper_run(argv[2], argv[3]));
    }
    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return CUP_STATUS_USAGE;
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_help(argv[0]);
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
        print_detailed_help(argv[0], command);
        return CUP_STATUS_SUCCESS;
    }

    /* Report the previous detached update result before enforcing the current journal policy. */
    (void)cup_update_result_report();

    if (help != NULL && strcmp(command, "help") != 0 && strcmp(command, "doctor") != 0 &&
        strcmp(command, "repair") != 0) {
        result = runtime_journal_require_none();
        if (result != CUP_OK) {
            return cup_error_to_exit_status(result);
        }
    }

    /* Install native interrupt handling only for commands that can perform long mutations. */
    if (command_uses_interrupt(argc, argv)) {
        result = interrupt_enable();
        if (result != CUP_OK) {
            fprintf(stderr, "Error: native interrupt handling could not be enabled.\n");
            return cup_error_to_exit_status(result);
        }
        interrupt_active = 1;
    }

    /* Public dispatch remains explicit so each parser keeps its typed command contract. */
    if (strcmp(command, "help") == 0) {
        result = parse_help(argv[0], argc, argv);
    } else if (strcmp(command, "search") == 0) {
        result = parse_optional_component(argv[0], command, argc, argv, command_search);
    } else if (strcmp(command, "list") == 0) {
        result = parse_optional_component(argv[0], command, argc, argv, command_list);
    } else if (strcmp(command, "install") == 0) {
        result = parse_install(argv[0], argc, argv);
    } else if (strcmp(command, "remove") == 0) {
        result = parse_component_entry(argv[0], command, argc, argv, command_remove);
    } else if (strcmp(command, "update") == 0) {
        result = parse_update(argv[0], argc, argv);
    } else if (strcmp(command, "config") == 0) {
        result = parse_config(argv[0], argc, argv);
    } else if (strcmp(command, "default") == 0) {
        result = parse_component_entry(argv[0], command, argc, argv, command_default);
    } else if (strcmp(command, "info") == 0) {
        result = parse_optional_component(argv[0], command, argc, argv, command_info);
    } else if (strcmp(command, "inspect") == 0) {
        result = parse_component_entry(argv[0], command, argc, argv, command_inspect);
    } else if (strcmp(command, "doctor") == 0) {
        result = parse_no_arguments(argv[0], command, argc, argv, command_doctor);
    } else if (strcmp(command, "repair") == 0) {
        result = parse_no_arguments(argv[0], command, argc, argv, command_repair);
    } else if (strcmp(command, "uninstall") == 0) {
        result = parse_uninstall(argv[0], argc, argv);
    } else {
        fprintf(stderr, "Error: unknown command '%s'.\n", command);
        print_usage(stderr, argv[0]);
        result = CUP_ERR_INVALID_INPUT;
    }

    if (interrupt_active) {
        interrupt_disable();
    }
    return cup_error_to_exit_status(result);
}

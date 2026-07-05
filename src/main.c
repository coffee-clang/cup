#include "commands.h"

#include "error.h"
#include "version.h"

#include <argtable3.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *details;
} CommandHelp;

static const CommandHelp COMMAND_HELP[] = {
    {"help", "help [command]", "Show general or command-specific help.",
        "Show the command list, or detailed usage for one command."},
    {"search", "search [<component>] [--target <target-platform>]",
        "Search the package catalog.",
        "Show packages declared by the manifest, optionally restricted to one component or target."},
    {"list", "list [<component>] [--target <target-platform>]",
        "List installed packages.",
        "Show locally installed packages, optionally restricted to one component or target."},
    {"install", "install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]",
        "Install one tool release.",
        "Resolve, verify, extract and commit one package. The first package in a scope becomes its default."},
    {"remove", "remove <component> <tool>@<release> [--target <target-platform>]",
        "Remove one installed release.",
        "Remove one installed package and reconcile its default and managed entry points."},
    {"update", "update <tool|component>",
        "Install stable releases for an installed tool or component.",
        "Keep older releases and move only defaults that referred to an older release of the same tool."},
    {"default", "default <component> <tool>@<release> [--target <target-platform>]",
        "Select one installed package as the default.",
        "Update one component, host and target scope and rebuild its managed entry points."},
    {"info", "info [<component>] [--target <target-platform>]",
        "Show active tools and entry points.",
        "Show configured defaults for the current host, optionally restricted to one component or target."},
    {"inspect", "inspect <component> <tool>@<release> [--target <target-platform>]",
        "Inspect an installed package.",
        "Validate one installed package and print its immutable info.txt metadata."},
    {"self-update", "self-update", "Update cup from an official verified release.",
        "Check the official release pointer, then download immutable versioned assets and schedule a transactional replacement."},
    {"doctor", "doctor", "Diagnose cup without modifying files.",
        "Check bootstrap assets, state, packages, transactions and entry points. This command is read-only."},
    {"repair", "repair", "Apply deterministic repairs.",
        "Recover interrupted operations and rebuild deterministic cup-managed data."},
    {"uninstall", "uninstall", "Remove cup and all managed data.",
        "Remove the canonical installation under ~/.cup without modifying PATH."}
};

static const char *program_name(const char *name) {
    return name == NULL ? "cup" : name;
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

static void print_command_usage(FILE *stream, const char *program,
    const CommandHelp *help) {
    fprintf(stream, "  %s %s\n", program_name(program), help->usage);
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
        fprintf(stdout, "  %-12s %s\n", COMMAND_HELP[i].name,
            COMMAND_HELP[i].summary);
    }
    fprintf(stdout,
        "\nEntry format:\n  <tool>@<release>\n"
        "\nExamples:\n"
        "  %s search compiler\n"
        "  %s install compiler gcc@stable\n"
        "  %s update compiler\n"
        "  %s default compiler gcc@stable\n"
        "  %s info\n"
        "  %s inspect compiler gcc@stable\n",
        program_name(program), program_name(program), program_name(program),
        program_name(program), program_name(program), program_name(program));
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

static CupError report_parse_error(const char *program, const char *command,
    struct arg_end *end, int errors) {
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

static CupError parse_optional_component(const char *program,
    const char *command, int argc, char **argv,
    CupError (*handler)(const char *, const char *)) {
    struct arg_str *component = arg_str0(NULL, NULL, "<component>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, target, end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result;

    if (errors != 0) {
        result = report_parse_error(program, command, end, errors);
    } else {
        result = handler(component->count ? component->sval[0] : NULL,
            target->count ? target->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_component_entry(const char *program,
    const char *command, int argc, char **argv,
    CupError (*handler)(const char *, const char *, const char *)) {
    struct arg_str *component = arg_str1(NULL, NULL, "<component>", NULL);
    struct arg_str *entry = arg_str1(NULL, NULL, "<tool>@<release>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, entry, target, end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result;

    if (errors != 0) {
        result = report_parse_error(program, command, end, errors);
    } else {
        result = handler(component->sval[0], entry->sval[0],
            target->count ? target->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_install(const char *program, int argc, char **argv) {
    struct arg_str *component = arg_str1(NULL, NULL, "<component>", NULL);
    struct arg_str *entry = arg_str1(NULL, NULL, "<tool>@<release>", NULL);
    struct arg_str *target = arg_str0(NULL, "target", "<target-platform>", NULL);
    struct arg_str *format = arg_str0("f", "format", "<archive-format>", NULL);
    struct arg_end *end = arg_end(8);
    void *table[] = {component, entry, target, format, end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result;

    if (errors != 0) {
        result = report_parse_error(program, "install", end, errors);
    } else {
        result = command_install(component->sval[0], entry->sval[0],
            target->count ? target->sval[0] : NULL,
            format->count ? format->sval[0] : NULL);
    }
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_update(const char *program, int argc, char **argv) {
    struct arg_str *selector = arg_str1(NULL, NULL, "<tool|component>", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {selector, end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result = errors == 0 ? command_update(selector->sval[0]) :
        report_parse_error(program, "update", end, errors);
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_no_arguments(const char *program, const char *command,
    int argc, char **argv, CupError (*handler)(void)) {
    struct arg_end *end = arg_end(4);
    void *table[] = {end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result = errors == 0 ? handler() :
        report_parse_error(program, command, end, errors);
    arg_freetable(table, sizeof(table) / sizeof(table[0]));
    return result;
}

static CupError parse_help(const char *program, int argc, char **argv) {
    struct arg_str *command = arg_str0(NULL, NULL, "[command]", NULL);
    struct arg_end *end = arg_end(4);
    void *table[] = {command, end};
    int errors = arg_parse(argc - 1, argv + 1, table);
    CupError result = CUP_OK;

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

int main(int argc, char *argv[]) {
    const char *command;

    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return CUP_ERR_INVALID_INPUT;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("cup %s\n", CUP_VERSION);
        return CUP_OK;
    }

    command = argv[1];
    if (strcmp(command, "help") == 0) {
        return parse_help(argv[0], argc, argv);
    }
    if (strcmp(command, "search") == 0) {
        return parse_optional_component(argv[0], command, argc, argv,
            command_search);
    }
    if (strcmp(command, "list") == 0) {
        return parse_optional_component(argv[0], command, argc, argv,
            command_list);
    }
    if (strcmp(command, "install") == 0) {
        return parse_install(argv[0], argc, argv);
    }
    if (strcmp(command, "remove") == 0) {
        return parse_component_entry(argv[0], command, argc, argv,
            command_remove);
    }
    if (strcmp(command, "update") == 0) {
        return parse_update(argv[0], argc, argv);
    }
    if (strcmp(command, "default") == 0) {
        return parse_component_entry(argv[0], command, argc, argv,
            command_default);
    }
    if (strcmp(command, "info") == 0) {
        return parse_optional_component(argv[0], command, argc, argv,
            command_info);
    }
    if (strcmp(command, "inspect") == 0) {
        return parse_component_entry(argv[0], command, argc, argv,
            command_inspect);
    }
    if (strcmp(command, "self-update") == 0) {
        return parse_no_arguments(argv[0], command, argc, argv,
            command_self_update);
    }
    if (strcmp(command, "doctor") == 0) {
        return parse_no_arguments(argv[0], command, argc, argv,
            command_doctor);
    }
    if (strcmp(command, "repair") == 0) {
        return parse_no_arguments(argv[0], command, argc, argv,
            command_repair);
    }
    if (strcmp(command, "uninstall") == 0) {
        return parse_no_arguments(argv[0], command, argc, argv,
            command_uninstall);
    }

    fprintf(stderr, "Error: unknown command '%s'.\n", command);
    print_usage(stderr, argv[0]);
    return CUP_ERR_INVALID_INPUT;
}

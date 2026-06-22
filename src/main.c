#include "commands.h"

#include "options.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    const char *details;
    const char *alternate_usage;
} CommandHelp;

static const CommandHelp COMMAND_HELP[] = {
    {"help", "help [command]",
        "Show general help or detailed help for one command.",
        "Show general help or detailed usage for one command.", NULL},
    {"list", "list [<component>] [--target <target-platform>]",
        "List installed packages.",
        "List installed packages for the current host and selected target, "
        "optionally restricted to one component.", NULL},
    {"install", "install <component> <tool>@<release> "
        "[--target <target-platform>] [--format|-f <archive-format>]",
        "Install a tool release.",
        "Install one concrete or symbolic stable release. The first package "
        "installed in a component/host/target scope becomes its default.", NULL},
    {"remove", "remove <component> <tool>@<release> "
        "[--target <target-platform>]",
        "Remove an installed tool release.",
        "Remove one installed release. Removing a default also removes its "
        "managed entry points.", NULL},
    {"update", "update <tool|component>",
        "Install the stable release of an installed tool or component.",
        "Install the stable release for every installed scope matching the "
        "tool or component. Older releases are retained. A default moves only "
        "when it still belongs to the updated tool.", NULL},
    {"default", "default <component> <tool>@<release> "
        "[--target <target-platform>]",
        "Select the default package for one scope.",
        "Select one installed package as the default for a component, host and "
        "target scope, then rebuild its managed entry points.", NULL},
    {"current", "current [<component>] [--target <target-platform>]",
        "Show the configured defaults.",
        "Show every valid default for the current host, optionally restricted "
        "to one component or target, together with its managed commands.", NULL},
    {"info", "info [<component> [<tool>@<release>]] "
        "[--target <target-platform>]",
        "Explore available packages or inspect an installed package.",
        "Without an entry, show the manifest catalog for the current host. "
        "With a component, restrict the catalog. With a tool release, validate "
        "the installed package and print its immutable info.txt metadata.", NULL},
    {"self-update", "self-update",
        "Update cup from the latest verified release.",
        "Compare the canonical installed version with the latest verified release "
        "and schedule a transactional replacement when an update exists.", NULL},
    {"doctor", "doctor",
        "Diagnose cup without modifying files.",
        "Check bootstrap assets, state, packages, transactions and entry points. "
        "This command is read-only.", NULL},
    {"repair", "repair",
        "Apply deterministic repairs to cup-managed data.",
        "Recover interrupted operations and apply deterministic repairs, including "
        "rebuilding managed entry points.", NULL},
    {"uninstall", "uninstall",
        "Remove cup and all cup-managed data.",
        "Remove the canonical cup installation and all data under ~/.cup.", NULL}
};

static const char *program_name(const char *name) {
    return name == NULL ? "cup" : name;
}

static const CommandHelp *find_command_help(const char *name) {
    size_t i;

    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        if (strcmp(COMMAND_HELP[i].name, name) == 0) {
            return &COMMAND_HELP[i];
        }
    }
    return NULL;
}

static void print_command_usage(FILE *stream, const char *prog_name,
    const CommandHelp *help) {
    prog_name = program_name(prog_name);
    fprintf(stream, "  %s %s\n", prog_name, help->usage);
    if (help->alternate_usage != NULL) {
        fprintf(stream, "  %s %s\n", prog_name, help->alternate_usage);
    }
}

static void print_usage(FILE *stream, const char *prog_name) {
    size_t i;

    fprintf(stream, "Usage:\n  %s --version\n", program_name(prog_name));
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        print_command_usage(stream, prog_name, &COMMAND_HELP[i]);
    }
}

static void print_help(const char *prog_name) {
    size_t i;

    print_usage(stdout, prog_name);
    fprintf(stdout, "\nCommands:\n");
    for (i = 0; i < sizeof(COMMAND_HELP) / sizeof(COMMAND_HELP[0]); ++i) {
        fprintf(stdout, "  %-12s %s\n", COMMAND_HELP[i].name,
            COMMAND_HELP[i].summary);
    }

    fprintf(stdout,
        "\nEntry format:\n"
        "  <tool>@<release>\n"
        "\nExamples:\n"
        "  %s install compiler gcc@stable\n"
        "  %s update gcc\n"
        "  %s update compiler\n"
        "  %s default compiler gcc@stable\n"
        "  %s current\n"
        "  %s help self-update\n",
        program_name(prog_name), program_name(prog_name),
        program_name(prog_name), program_name(prog_name),
        program_name(prog_name), program_name(prog_name));
}

static int print_command_help(const char *prog_name, const char *command) {
    const CommandHelp *help = find_command_help(command);

    if (help == NULL) {
        return 0;
    }

    fprintf(stdout, "Usage:\n");
    print_command_usage(stdout, prog_name, help);
    fprintf(stdout, "\n%s\n", help->details);
    return 1;
}

static CupError parse_options_for_command(int first_option, int argc,
    char *const argv[], OptionFlags allowed, const char *command,
    CommandOptions *options) {
    CupError err;

    err = options_parse(first_option, argc, argv, options);
    if (err != CUP_OK) {
        return err;
    }

    return options_validate(options, allowed, command);
}

int main(int argc, char *argv[]) {
    CupError err;
    CommandOptions options;
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
        if (argc == 2) {
            print_help(argv[0]);
            return CUP_OK;
        }
        if (argc == 3 && print_command_help(argv[0], argv[2])) {
            return CUP_OK;
        }

        if (argc == 3) {
            fprintf(stderr, "Error: unknown command '%s'.\n", argv[2]);
        } else {
            fprintf(stderr, "Error: command 'help' accepts at most one command name.\n");
        }
        return CUP_ERR_INVALID_INPUT;
    }

    if (strcmp(command, "list") == 0) {
        const char *component = NULL;
        int first_option = 2;

        if (argc > 2 && argv[2][0] != '-') {
            component = argv[2];
            first_option = 3;
        }
        err = parse_options_for_command(first_option, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_command_help(argv[0], command);
            return err;
        }
        return command_list(component, options.target);
    }

    if (strcmp(command, "install") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'install'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        err = parse_options_for_command(4, argc, argv,
            OPT_FORMAT | OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }
        return command_install(argv[2], argv[3], options.target, options.format);
    }

    if (strcmp(command, "remove") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'remove'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        err = parse_options_for_command(4, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }
        return command_remove(argv[2], argv[3], options.target);
    }

    if (strcmp(command, "update") == 0) {
        if (argc != 3) {
            fprintf(stderr, "Error: command 'update' requires one tool or component.\n");
            print_command_help(argv[0], command);
            return CUP_ERR_INVALID_INPUT;
        }
        return command_update(argv[2]);
    }

    if (strcmp(command, "default") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: command 'default' requires a component and "
                "an installed tool release.\n");
            print_command_help(argv[0], command);
            return CUP_ERR_INVALID_INPUT;
        }
        err = parse_options_for_command(4, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_command_help(argv[0], command);
            return err;
        }
        return command_default(argv[2], argv[3], options.target);
    }

    if (strcmp(command, "current") == 0) {
        if (argc == 2 || (argc > 2 && argv[2][0] == '-')) {
            err = parse_options_for_command(2, argc, argv,
                OPT_TARGET, command, &options);
            if (err != CUP_OK) {
                print_command_help(argv[0], command);
                return err;
            }
            return command_current(NULL, options.target);
        }
        err = parse_options_for_command(3, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_command_help(argv[0], command);
            return err;
        }
        return command_current(argv[2], options.target);
    }

    if (strcmp(command, "info") == 0) {
        const char *component = NULL;
        const char *entry = NULL;
        int first_option = 2;

        if (argc > 2 && argv[2][0] != '-') {
            component = argv[2];
            first_option = 3;
            if (argc > 3 && argv[3][0] != '-') {
                entry = argv[3];
                first_option = 4;
            }
        }
        err = parse_options_for_command(first_option, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_command_help(argv[0], command);
            return err;
        }
        return command_info(component, entry, options.target);
    }

    if (strcmp(command, "self-update") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'self-update' does not accept arguments.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        return command_self_update();
    }

    if (strcmp(command, "doctor") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'doctor' does not accept arguments.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        return command_doctor();
    }

    if (strcmp(command, "repair") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'repair' does not accept arguments.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        return command_repair();
    }

    if (strcmp(command, "uninstall") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'uninstall' does not accept arguments.\n");
            return CUP_ERR_INVALID_INPUT;
        }
        return command_uninstall();
    }

    fprintf(stderr, "Error: unknown command '%s'.\n", command);
    print_usage(stderr, argv[0]);
    return CUP_ERR_INVALID_INPUT;
}

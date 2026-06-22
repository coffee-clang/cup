#include "commands.h"

#include "options.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *stream, const char *prog_name) {
    if (prog_name == NULL) {
        prog_name = "cup";
    }

    fprintf(stream,
        "Usage:\n"
        "  %s --version\n"
        "  %s help [command]\n"
        "  %s list [--target <target-platform>]\n"
        "  %s install <component> <tool>@<release> "
        "[--target <target-platform>] [--format|-f <archive-format>]\n"
        "  %s remove <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s update <tool|component>\n"
        "  %s default [--target <target-platform>]\n"
        "  %s default <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s current [<component>] [--target <target-platform>]\n"
        "  %s info <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s self-update\n"
        "  %s doctor\n"
        "  %s repair\n"
        "  %s uninstall\n",
        prog_name, prog_name, prog_name, prog_name, prog_name,
        prog_name, prog_name, prog_name, prog_name, prog_name,
        prog_name, prog_name, prog_name, prog_name);
}

static void print_help(const char *prog_name) {
    print_usage(stdout, prog_name);

    fprintf(stdout,
        "\n"
        "Commands:\n"
        "  help         Show general help or detailed help for one command.\n"
        "  list         List installed packages for one host/target scope.\n"
        "  install      Install a tool release.\n"
        "  remove       Remove an installed tool release.\n"
        "  update       Install the stable release of an installed tool or component.\n"
        "  default      List defaults or select one installed package as default.\n"
        "  current      Show one component default or all current defaults.\n"
        "  info         Show metadata for an installed package.\n"
        "  self-update  Update the installed cup executable from the bootstrap release.\n"
        "  doctor       Diagnose cup without modifying files.\n"
        "  repair       Apply deterministic repairs to cup-managed data.\n"
        "  uninstall    Remove cup and all cup-managed data.\n"
        "\n"
        "Entry format:\n"
        "  <tool>@<release>\n"
        "\n"
        "Examples:\n"
        "  %s install compiler gcc@stable\n"
        "  %s update gcc\n"
        "  %s update compiler\n"
        "  %s default compiler gcc@stable\n"
        "  %s current\n"
        "  %s help self-update\n",
        prog_name, prog_name, prog_name, prog_name, prog_name, prog_name);
}

static int print_command_help(const char *prog_name, const char *command) {
    if (strcmp(command, "help") == 0) {
        fprintf(stdout,
            "Usage: %s help [command]\n\n"
            "Show general help or detailed usage for one command.\n",
            prog_name);
    } else if (strcmp(command, "list") == 0) {
        fprintf(stdout,
            "Usage: %s list [--target <target-platform>]\n\n"
            "List installed packages for the current host and selected target.\n",
            prog_name);
    } else if (strcmp(command, "install") == 0) {
        fprintf(stdout,
            "Usage: %s install <component> <tool>@<release> "
            "[--target <target-platform>] [--format|-f <archive-format>]\n\n"
            "Install one concrete or symbolic stable release. The first package "
            "installed in a component/host/target scope becomes its default.\n",
            prog_name);
    } else if (strcmp(command, "remove") == 0) {
        fprintf(stdout,
            "Usage: %s remove <component> <tool>@<release> "
            "[--target <target-platform>]\n\n"
            "Remove one installed release. Removing a default also removes its "
            "managed entry points.\n",
            prog_name);
    } else if (strcmp(command, "update") == 0) {
        fprintf(stdout,
            "Usage: %s update <tool|component>\n\n"
            "Install the stable release for every installed scope matching the "
            "tool or component. Older releases are retained. A default moves only "
            "when it belonged to the updated tool.\n",
            prog_name);
    } else if (strcmp(command, "default") == 0) {
        fprintf(stdout,
            "Usage:\n"
            "  %s default [--target <target-platform>]\n"
            "  %s default <component> <tool>@<release> "
            "[--target <target-platform>]\n\n"
            "Without an entry, list defaults for the current host. With an entry, "
            "select an installed package and rebuild managed entry points.\n",
            prog_name, prog_name);
    } else if (strcmp(command, "current") == 0) {
        fprintf(stdout,
            "Usage: %s current [<component>] [--target <target-platform>]\n\n"
            "Show the selected component default, or list all defaults for the "
            "current host when no component is supplied.\n",
            prog_name);
    } else if (strcmp(command, "info") == 0) {
        fprintf(stdout,
            "Usage: %s info <component> <tool>@<release> "
            "[--target <target-platform>]\n\n"
            "Validate an installed package and print its immutable info.txt metadata.\n",
            prog_name);
    } else if (strcmp(command, "self-update") == 0) {
        fprintf(stdout,
            "Usage: %s self-update\n\n"
            "Compare the canonical installed executable with the latest verified "
            "bootstrap assets and schedule a verified replacement when an update exists.\n",
            prog_name);
    } else if (strcmp(command, "doctor") == 0) {
        fprintf(stdout,
            "Usage: %s doctor\n\n"
            "Check bootstrap assets, state, packages, transactions and entry points. "
            "This command is read-only.\n",
            prog_name);
    } else if (strcmp(command, "repair") == 0) {
        fprintf(stdout,
            "Usage: %s repair\n\n"
            "Recover interrupted operations and apply deterministic repairs, "
            "including rebuilding managed entry points.\n",
            prog_name);
    } else if (strcmp(command, "uninstall") == 0) {
        fprintf(stdout,
            "Usage: %s uninstall\n\n"
            "Remove the canonical cup installation and all data under ~/.cup.\n",
            prog_name);
    } else {
        return 0;
    }

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
        err = parse_options_for_command(2, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }
        return command_list(options.target);
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
        if (argc == 2 || (argc > 2 && argv[2][0] == '-')) {
            err = parse_options_for_command(2, argc, argv,
                OPT_TARGET, command, &options);
            if (err != CUP_OK) {
                print_command_help(argv[0], command);
                return err;
            }
            return command_default(NULL, NULL, options.target);
        }
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'default'.\n");
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
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'info'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        err = parse_options_for_command(4, argc, argv,
            OPT_TARGET, command, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }
        return command_info(argv[2], argv[3], options.target);
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

#include "commands.h"

#include "options.h"

#include <stdio.h>
#include <string.h>

// CLI OUTPUT
static void print_usage(FILE *stream, const char *prog_name) {
    if (prog_name == NULL) {
        prog_name = "cup";
    }

    fprintf(stream,
        "Usage:\n"
        "  %s help\n"
        "  %s list [--target <target-platform>]\n"
        "  %s install <component> <tool>@<release> [--target <target-platform>] [--format|-f <archive-format>]\n"
        "  %s remove <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s default <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s current <component> [--target <target-platform>]\n"
        "  %s info <component> <tool>@<release> [--target <target-platform>]\n"
        "  %s doctor\n"
        "  %s repair\n"
        "  %s uninstall\n",
        prog_name, prog_name, prog_name, prog_name, prog_name,
        prog_name, prog_name, prog_name, prog_name, prog_name);
}

static void print_help(const char *prog_name) {
    print_usage(stdout, prog_name);

    fprintf(stdout,
        "\n"
        "Commands:\n"
        "  help       Show this help message.\n"
        "  list       List installed components for the current host and selected target.\n"
        "  install    Install a tool release for a component.\n"
        "  remove     Remove an installed tool release.\n"
        "  default    Set the default installed tool release for a component.\n"
        "  current    Show the current default for a component.\n"
        "  info       Show metadata for an installed package.\n"
        "  doctor     Check cup state and installation consistency without modifying files.\n"
        "  repair     Recover interrupted operations and repair safe cup inconsistencies.\n"
        "  uninstall  Remove cup itself and all cup-managed data.\n"
        "\n"
        "Entry format:\n"
        "  <tool>@<release>\n"
        "\n"
        "Examples:\n"
        "  %s install compiler gcc@stable\n"
        "  %s install compiler gcc@stable --target windows-x64\n"
        "  %s default compiler gcc@stable\n"
        "  %s current compiler\n"
        "  %s info compiler gcc@stable\n",
        prog_name, prog_name, prog_name, prog_name, prog_name);
}

// COMMAND DISPATCH
int main(int argc, char *argv[]) {
    CupError err;
    CommandOptions options;
    const char *command;
    int start_option;

    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return CUP_ERR_INVALID_INPUT;
    }

    command = argv[1];

    if (strcmp(command, "help") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'help' does not accept arguments.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        print_help(argv[0]);
        return CUP_OK;
    }

    if (strcmp(command, "list") == 0) {
        start_option = 2;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_list(options.target);
    }

    if (strcmp(command, "install") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'install'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_FORMAT | OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_install(argv[2], argv[3], options.target, options.format);
    }

    if (strcmp(command, "remove") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'remove'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_remove(argv[2], argv[3], options.target);
    }

    if (strcmp(command, "default") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'default'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_default(argv[2], argv[3], options.target);
    }

    if (strcmp(command, "current") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing arguments for command 'current'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 3;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_current(argv[2], options.target);
    }

    if (strcmp(command, "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'info'.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_TARGET, command);
        if (err != CUP_OK) {
            print_usage(stderr, argv[0]);
            return err;
        }

        return handle_info(argv[2], argv[3], options.target);
    }

    if (strcmp(command, "doctor") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'doctor' does not accept arguments.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        return handle_doctor();
    }

    if (strcmp(command, "repair") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'repair' does not accept arguments.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        return handle_repair();
    }

    if (strcmp(command, "uninstall") == 0) {
        if (argc != 2) {
            fprintf(stderr, "Error: command 'uninstall' does not accept arguments.\n");
            print_usage(stderr, argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        return handle_uninstall();
    }

    fprintf(stderr, "Error: unknown command '%s'.\n", command);
    print_usage(stderr, argv[0]);
    return CUP_ERR_INVALID_INPUT;
}

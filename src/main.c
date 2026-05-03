#include "commands.h"
#include "filesystem.h"
#include "options.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog_name) {
    if (prog_name == NULL) {
        prog_name = "./cup";
    }

    fprintf(stderr,
        "Usage:\n"
        "  %s list [--platform <platform>]\n"
        "  %s install <component> <tool>@<release> [--format|-f <archive-format>] [--platform <platform>]\n"
        "  %s remove <component> <tool>@<release> [--platform <platform>]\n"
        "  %s default <component> <tool>@<release> [--platform <platform>]\n"
        "  %s current <component> [--platform <platform>]\n",
        prog_name, prog_name, prog_name, prog_name, prog_name);
}

int main(int argc, char *argv[]) {
    CupError err;
    CommandOptions options;
    const char *command;
    int start_option;

    if (argc < 2) {
        print_usage(argv[0]);
        return CUP_ERR_INVALID_INPUT;
    }

    err = cleanup_all_tmp();
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: could not clean temporary installation directories.\n");
    }

    command = argv[1];

    if (strcmp(command, "list") == 0) {
        start_option = 2;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_PLATFORM, command);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        return handle_list(options.platform);
    }

    if (strcmp(command, "install") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'install'.\n");
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_FORMAT | OPT_PLATFORM, command);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        return handle_install(argv[2], argv[3], options.format, options.platform);
    }

    if (strcmp(command, "remove") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'remove'.\n");
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_PLATFORM, command);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        return handle_remove(argv[2], argv[3], options.platform);
    }

    if (strcmp(command, "default") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Error: missing arguments for command 'default'.\n");
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 4;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_PLATFORM, command);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        return handle_default(argv[2], argv[3], options.platform);
    }

    if (strcmp(command, "current") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing arguments for command 'current'.\n");
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        start_option = 3;

        err = parse_command_options(start_option, argc, argv, &options);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }

        err = validate_command_options(&options, OPT_PLATFORM, command);
        if (err != CUP_OK) {
            print_usage(argv[0]);
            return err;
        }
        
        return handle_current(argv[2], options.platform);
    }

    fprintf(stderr, "Error: unknown command '%s'.\n", command);
    print_usage(argv[0]);
    return CUP_ERR_INVALID_INPUT;
}
#include <stdio.h>
#include <string.h>

#include "component.h"
#include "fs.h"
#include "error.h"

static void print_usage(const char *prog_name) {
    fprintf(stderr,
        "Usage:\n"
        "  %s list\n"
        "  %s install <component> <tool>@<release> [--format <archive-format>]\n"
        "  %s install <component> <tool>@<release> [-f <archive-format>]\n"
        "  %s remove <component> <tool>@<release>\n"
        "  %s default <component> <tool>@<release>\n"
        "  %s current <component>\n",
        prog_name, prog_name, prog_name, prog_name, prog_name, prog_name);
}

int main(int argc, char *argv[]) {
    CupError err;
    const char *command;
    const char *format_override = NULL;

    err = cleanup_all_tmp();
    if (err != CUP_OK) {
        fprintf(stderr, "Warning: could not clean temporary installation directories.\n");
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return CUP_ERR_INVALID_INPUT;
    }

    command = argv[1];

    if (strcmp(command, "list") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        return handle_list();
    }

    if (strcmp(command, "install") == 0) {
        int valid_install_args = 1;

        if (argc == 6) {
            if (strcmp(argv[4], "--format") == 0 || strcmp(argv[4], "-f") == 0) {
                format_override = argv[5];
            } else {
                valid_install_args = 0;
            }
        } else if (argc != 4) {
            valid_install_args = 0;
        }

        if (!valid_install_args) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }

        return handle_install(argv[2], argv[3], format_override);
    }

    if (strcmp(command, "remove") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        return handle_remove(argv[2], argv[3]);
    }

    if (strcmp(command, "default") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        return handle_default(argv[2], argv[3]);
    }

    if (strcmp(command, "current") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        return handle_current(argv[2]);
    }

    fprintf(stderr, "Error: unknown command '%s'.\n", command);
    print_usage(argv[0]);
    return CUP_ERR_INVALID_INPUT;
}
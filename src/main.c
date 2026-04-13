#include <stdio.h>
#include <string.h>

#include "component.h"
#include "fs.h"
#include "error.h"

static void print_usage(const char *prog_name) {
    fprintf(stderr,
        "Usage:\n"
        "  %s list\n"
        "  %s install <component> <tool>@<release>\n"
        "  %s remove <component> <tool>@<release>\n"
        "  %s default <component> <tool>@<release>\n"
        "  %s current <component>\n",
        prog_name, prog_name, prog_name, prog_name, prog_name);
}

int main(int argc, char *argv[]) {
    const char *command;
    CupError err;

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
        if (argc != 4) {
            print_usage(argv[0]);
            return CUP_ERR_INVALID_INPUT;
        }
        return handle_install(argv[2], argv[3]);
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
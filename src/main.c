#include <stdio.h>
#include <string.h>
#include "toolchain.h"

static void print_usage(const char *prog_name){
    fprintf(stderr,
        "Usage:\n"
        "  %s list\n"
        "  %s install <name>@<version>\n"
        "  %s remove <name>@<version>\n"
        "  %s default <name>@<version>\n"
        "  %s current\n",
        prog_name, prog_name, prog_name, prog_name, prog_name);
}

int main(int argc, char *argv[]){
    const char *command;
    
    if(argc < 2){
        print_usage(argv[0]);
        return 1;
    }

    command = argv[1];

    if(strcmp(command, "list") == 0){
        if(argc != 2){
            print_usage(argv[0]);
            return 1;
        }
        return handle_list();
    }

    if(strcmp(command, "install") == 0){
        if(argc != 3){
            print_usage(argv[0]);
            return 1;
        }
        return handle_install(argv[2]);
    }

    if(strcmp(command, "remove") == 0){
        if(argc != 3){
            print_usage(argv[0]);
            return 1;
        }
        return handle_remove(argv[2]);
    }

    if(strcmp(command, "default") == 0){
        if(argc != 3){
            print_usage(argv[0]);
            return 1;
        }
        return handle_default(argv[2]);
    }

    if(strcmp(command, "current") == 0){
        if(argc != 2){
            print_usage(argv[0]);
            return 1;
        }
        return handle_current();
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    print_usage(argv[0]);
    return 1;
}